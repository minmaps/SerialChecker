// TPM: device properties (TBS) and the endorsement key read three ways: straight from the TPM, through the
// Platform Crypto Provider (what Get-TpmEndorsementKeyInfo uses) and from the copy Windows caches in the registry.
#include "hw/Util.h"

#include <windows.h>
#include <wincrypt.h>
#include <ncrypt.h>
#include <tbs.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>

namespace hw {
namespace {

constexpr uint32_t kCcReadPublic = 0x173;
constexpr uint32_t kCcGetCapability = 0x17A;
constexpr uint32_t kCapTpmProperties = 6;
constexpr uint32_t kPtFamily = 0x100, kPtRevision = 0x102, kPtManufacturer = 0x105, kPtVendorString1 = 0x106,
                   kPtFirmware1 = 0x10B, kPtFirmware2 = 0x10C;
constexpr uint16_t kAlgRsa = 0x0001, kAlgNull = 0x0010, kAlgRsaes = 0x0015;
constexpr uint32_t kEkHandle = 0x81010001;  // TCG persistent handle of the RSA EK
constexpr const wchar_t* kTpmWmiKey = L"SYSTEM\\CurrentControlSet\\Services\\TPM\\WMI";
constexpr const wchar_t* kEndorsementKey = L"SYSTEM\\CurrentControlSet\\Services\\TPM\\WMI\\Endorsement";

// TPM responses are big-endian; reading past the end clears ok and yields zeros.
struct Reader {
    const BYTE* p;
    const BYTE* end;
    bool ok = true;

    const BYTE* Bytes(size_t n)
    {
        if ((size_t)(end - p) < n) {
            ok = false;
            p = end;
            return nullptr;
        }
        const BYTE* r = p;
        p += n;
        return r;
    }
    uint32_t Uint(size_t n)
    {
        const BYTE* b = Bytes(n);
        uint32_t v = 0;
        for (size_t i = 0; b && i < n; ++i) v = v << 8 | b[i];
        return v;
    }
    uint16_t U16() { return (uint16_t)Uint(2); }
    uint32_t U32() { return Uint(4); }
};

void Put32(std::vector<BYTE>& b, uint32_t v)
{
    for (int s = 24; s >= 0; s -= 8) b.push_back((BYTE)(v >> s));
}

// Sends a TPM 2.0 command without sessions (every parameter is a UINT32); returns the response parameters.
std::vector<BYTE> Submit(TBS_HCONTEXT ctx, uint32_t code, std::initializer_list<uint32_t> args)
{
    std::vector<BYTE> cmd = { 0x80, 0x01 };  // TPM_ST_NO_SESSIONS
    Put32(cmd, (uint32_t)(10 + 4 * args.size()));
    Put32(cmd, code);
    for (uint32_t a : args) Put32(cmd, a);

    std::vector<BYTE> resp(4096);
    UINT32 len = (UINT32)resp.size();
    if (Tbsip_Submit_Command(ctx, TBS_COMMAND_LOCALITY_ZERO, TBS_COMMAND_PRIORITY_NORMAL, cmd.data(), (UINT32)cmd.size(),
                             resp.data(), &len) != TBS_SUCCESS || len < 10)
        return {};
    Reader r{ resp.data() + 6, resp.data() + len };
    if (r.U32() != 0) return {};  // TPM_RC_SUCCESS
    return std::vector<BYTE>(resp.begin() + 10, resp.begin() + len);
}

// TPM2_GetCapability(TPM_CAP_TPM_PROPERTIES) over the fixed properties.
std::map<uint32_t, uint32_t> FixedProperties(TBS_HCONTEXT ctx)
{
    std::map<uint32_t, uint32_t> props;
    std::vector<BYTE> resp = Submit(ctx, kCcGetCapability, { kCapTpmProperties, kPtFamily, 0x30 });
    Reader r{ resp.data(), resp.data() + resp.size() };
    r.Bytes(1);  // moreData
    if (r.U32() != kCapTpmProperties) return props;
    uint32_t count = r.U32();
    for (uint32_t i = 0; i < count && r.ok; ++i) {
        uint32_t tag = r.U32(), value = r.U32();
        if (r.ok) props[tag] = value;
    }
    return props;
}

// Four ASCII characters packed in a property value ("INTC", "2.0"...).
std::string Chars(uint32_t v)
{
    std::string s;
    for (int sh = 24; sh >= 0; sh -= 8)
        if (char c = (char)(v >> sh); c >= 0x20 && c < 0x7F) s += c;
    return s;
}

// TCG vendor ID registry.
const char* VendorName(const std::string& id)
{
    static const struct { const char* id; const char* name; } vendors[] = {
        { "AMD", "AMD" },        { "INTC", "Intel" },     { "IFX", "Infineon" },   { "NTC", "Nuvoton" },
        { "STM", "STMicroelectronics" }, { "MSFT", "Microsoft" }, { "QCOM", "Qualcomm" }, { "ATML", "Atmel" },
        { "BRCM", "Broadcom" },  { "CSCO", "Cisco" },     { "FLYS", "Flyslice" },  { "GOOG", "Google" },
        { "HISI", "Huawei" },    { "HPE", "HPE" },        { "IBM", "IBM" },        { "LEN", "Lenovo" },
        { "NSM", "National Semiconductor" }, { "NTZ", "Nationz" }, { "ROCC", "Rockchip" }, { "SMSC", "SMSC" },
        { "SMSN", "Samsung" },   { "SNS", "Sinosun" },    { "TXN", "Texas Instruments" }, { "WEC", "Winbond" },
    };
    for (const auto& v : vendors)
        if (id == v.id) return v.name;
    return nullptr;
}

std::vector<BYTE> RsaBlob(uint32_t bits, uint32_t exponent, const BYTE* modulus, size_t modulusSize)
{
    BYTE exp[4];
    ULONG n = 0;
    for (int s = 24; s >= 0; s -= 8)
        if (n || (BYTE)(exponent >> s)) exp[n++] = (BYTE)(exponent >> s);
    BCRYPT_RSAKEY_BLOB hdr{ BCRYPT_RSAPUBLIC_MAGIC, bits, n, (ULONG)modulusSize, 0, 0 };
    std::vector<BYTE> blob((const BYTE*)&hdr, (const BYTE*)(&hdr + 1));
    blob.insert(blob.end(), exp, exp + n);
    blob.insert(blob.end(), modulus, modulus + modulusSize);
    return blob;
}

// TPM2_ReadPublic of a persistent RSA key, rebuilt as the BCRYPT_RSAKEY_BLOB the PCP provider returns.
std::vector<BYTE> ReadPublicRsa(TBS_HCONTEXT ctx, uint32_t handle)
{
    std::vector<BYTE> resp = Submit(ctx, kCcReadPublic, { handle });
    Reader r{ resp.data(), resp.data() + resp.size() };
    r.U16();                                              // TPM2B_PUBLIC size
    if (r.U16() != kAlgRsa) return {};
    r.U16();                                              // nameAlg
    r.U32();                                              // objectAttributes
    r.Bytes(r.U16());                                     // authPolicy
    if (r.U16() != kAlgNull) r.Bytes(4);                  // symmetric: keyBits + mode
    if (uint16_t s = r.U16(); s != kAlgNull && s != kAlgRsaes) r.U16();  // scheme hashAlg
    uint16_t bits = r.U16();
    uint32_t exponent = r.U32();
    uint16_t size = r.U16();
    const BYTE* modulus = r.Bytes(size);
    if (!r.ok || !size) return {};
    return RsaBlob(bits, exponent ? exponent : 65537, modulus, size);
}

std::string LowerHex(const BYTE* data, size_t size)
{
    std::string s = HexBytes(data, size, 0);
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// SHA-256 of the DER RSAPublicKey, as Get-TpmEndorsementKeyInfo -HashAlgorithm sha256 prints it.
std::string RsaKeyHash(const std::vector<BYTE>& blob)
{
    auto* hdr = (const BCRYPT_RSAKEY_BLOB*)blob.data();
    if (blob.size() < sizeof(*hdr) || hdr->Magic != BCRYPT_RSAPUBLIC_MAGIC ||
        blob.size() < sizeof(*hdr) + hdr->cbPublicExp + hdr->cbModulus)
        return {};
    BYTE* der = nullptr;
    DWORD derSize = 0;
    if (!CryptEncodeObjectEx(X509_ASN_ENCODING, CNG_RSA_PUBLIC_KEY_BLOB, blob.data(), CRYPT_ENCODE_ALLOC_FLAG, nullptr,
                             &der, &derSize))
        return {};
    BYTE hash[32];
    DWORD hashSize = sizeof(hash);
    BOOL ok = CryptHashCertificate2(BCRYPT_SHA256_ALGORITHM, 0, nullptr, der, derSize, hash, &hashSize);
    LocalFree(der);
    return ok ? LowerHex(hash, hashSize) : std::string();
}

Field KeyHashField(std::string label, const std::vector<BYTE>& blob)
{
    std::string hash = RsaKeyHash(blob);
    return hash.empty() ? Unsupported(std::move(label), "Not available") : MakeField(std::move(label), hash);
}

std::vector<BYTE> PcpProperty(NCRYPT_PROV_HANDLE prov, const wchar_t* name)
{
    DWORD size = 0;
    if (!prov || NCryptGetProperty(prov, name, nullptr, 0, &size, 0) != ERROR_SUCCESS || !size) return {};
    std::vector<BYTE> buf(size);
    if (NCryptGetProperty(prov, name, buf.data(), size, &size, 0) != ERROR_SUCCESS) return {};
    buf.resize(size);
    return buf;
}

// PCP_EKCERT hands back an HCERTSTORE (certificates from TPM NV and those Windows downloaded); a raw encoded
// certificate is accepted too.
void AddEkCertificate(NCRYPT_PROV_HANDLE prov, Card& card)
{
    std::vector<BYTE> data = PcpProperty(prov, NCRYPT_PCP_EKCERT_PROPERTY);
    HCERTSTORE store = nullptr;
    PCCERT_CONTEXT cert = nullptr;
    if (data.size() == sizeof(store)) {
        memcpy(&store, data.data(), sizeof(store));
        if (store) cert = CertEnumCertificatesInStore(store, nullptr);
    } else if (!data.empty()) {
        cert = CertCreateCertificateContext(X509_ASN_ENCODING, data.data(), (DWORD)data.size());
    }

    if (!cert) {
        card.fields.push_back(Field{ "EK Certificate", "Not present", true });
    } else {
        // Serial is stored little-endian; print it the way certificate viewers do.
        const CRYPT_INTEGER_BLOB& sn = cert->pCertInfo->SerialNumber;
        std::vector<BYTE> serial(sn.pbData, sn.pbData + sn.cbData);
        std::reverse(serial.begin(), serial.end());
        card.fields.push_back(MakeField("EK Cert Serial", HexBytes(serial.data(), serial.size(), 0)));
        wchar_t issuer[256]{};
        CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, CERT_NAME_ISSUER_FLAG, nullptr, issuer, _countof(issuer));
        card.fields.push_back(MakeField("EK Cert Issuer", Narrow(issuer)));
        CertFreeCertificateContext(cert);
    }
    if (store) CertCloseStore(store, 0);
}

} // namespace

void CollectTpm(std::vector<Section>& out)
{
    Section section{ "TPM" };
    TPM_DEVICE_INFO info{};
    if (Tbsi_GetDeviceInfo(sizeof(info), &info) != TBS_SUCCESS) {
        section.cards.push_back({ "TPM", "TBS", { Unsupported("Status", "No TPM found") } });
        out.push_back(std::move(section));
        return;
    }

    TBS_HCONTEXT ctx = 0;
    TBS_CONTEXT_PARAMS2 params{};
    params.version = TBS_CONTEXT_VERSION_TWO;
    params.includeTpm20 = 1;
    if (info.tpmVersion != TPM_VERSION_20 || Tbsi_Context_Create((PCTBS_CONTEXT_PARAMS)&params, &ctx) != TBS_SUCCESS)
        ctx = 0;
    NCRYPT_PROV_HANDLE prov = 0;
    if (NCryptOpenStorageProvider(&prov, MS_PLATFORM_CRYPTO_PROVIDER, 0) != ERROR_SUCCESS) prov = 0;

    Card dev{ "Trusted Platform Module", "TBS" };
    std::map<uint32_t, uint32_t> props = ctx ? FixedProperties(ctx) : std::map<uint32_t, uint32_t>{};
    if (props.empty()) {
        dev.fields.push_back(MakeField("Spec Version", info.tpmVersion == TPM_VERSION_12   ? "1.2"
                                                       : info.tpmVersion == TPM_VERSION_20 ? "2.0"
                                                                                           : ""));
        dev.fields.push_back(Unsupported("TPM2_GetCapability", "Not available"));
    } else {
        std::string id = Trim(Chars(props[kPtManufacturer]));
        const char* name = VendorName(id);
        dev.fields.push_back(MakeField("Manufacturer", name && id != name ? id + "  (" + name + ")" : id));
        std::string vendor;
        for (uint32_t i = 0; i < 4; ++i) vendor += Chars(props[kPtVendorString1 + i]);
        dev.fields.push_back(MakeField("Vendor String", vendor));
        uint32_t fw1 = props[kPtFirmware1], fw2 = props[kPtFirmware2];
        dev.fields.push_back(MakeField("Firmware", Format("%u.%u.%u.%u", fw1 >> 16, fw1 & 0xFFFF, fw2 >> 16, fw2 & 0xFFFF)));
        uint32_t rev = props[kPtRevision];
        dev.fields.push_back(MakeField("Spec Version", Format("%s, revision %u.%02u", Chars(props[kPtFamily]).c_str(),
                                                              rev / 100, rev % 100)));
    }
    section.cards.push_back(std::move(dev));

    std::vector<BYTE> tpmEk = ctx ? ReadPublicRsa(ctx, kEkHandle) : std::vector<BYTE>{};
    std::vector<BYTE> pcpEk = PcpProperty(prov, NCRYPT_PCP_EKPUB_PROPERTY);
    const std::vector<BYTE>& any = tpmEk.empty() ? pcpEk : tpmEk;
    std::string badge = any.size() >= sizeof(BCRYPT_RSAKEY_BLOB)
                            ? Format("RSA %lu", ((const BCRYPT_RSAKEY_BLOB*)any.data())->BitLength)
                            : "EK";
    Card ek{ "Endorsement Key", badge };
    ek.fields.push_back(KeyHashField("TPM2_ReadPublic", tpmEk));
    ek.fields.push_back(KeyHashField("PCP_EKPUB", pcpEk));
    ek.fields.push_back(KeyHashField("EkPub [Registry]", RegReadBinary(kEndorsementKey, L"EkPub")));
    AddEkCertificate(prov, ek);
    section.cards.push_back(std::move(ek));

    // Both are recreated when the TPM is cleared, unlike the EK.
    Card keys{ "Windows Keys", "PCP" };
    keys.fields.push_back(KeyHashField("SRK Public Key", PcpProperty(prov, NCRYPT_PCP_SRKPUB_PROPERTY)));
    std::vector<BYTE> aik = RegReadBinary(kTpmWmiKey, L"WindowsAIKHash");
    keys.fields.push_back(aik.empty() ? Unsupported("AIK Hash [Registry]", "Not set")
                                      : MakeField("AIK Hash [Registry]", LowerHex(aik.data(), aik.size())));
    section.cards.push_back(std::move(keys));

    if (prov) NCryptFreeObject(prov);
    if (ctx) Tbsip_Context_Close(ctx);
    out.push_back(std::move(section));
}

} // namespace hw
