#include "repair_core.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace ziprepair {
namespace {

constexpr uint32_t LOCAL_SIG = 0x04034b50;
constexpr uint32_t CENTRAL_SIG = 0x02014b50;
constexpr uint32_t EOCD_SIG = 0x06054b50;
constexpr uint32_t ZIP64_EOCD_SIG = 0x06064b50;
constexpr uint32_t ZIP64_LOCATOR_SIG = 0x07064b50;
constexpr size_t COPY_BUFFER_SIZE = 4 * 1024 * 1024;

std::wstring nativePath(const std::wstring& path) {
    if (path.rfind(LR"(\\?\)", 0) == 0) return path;
    if (path.rfind(LR"(\\)", 0) == 0) return LR"(\\?\UNC\)" + path.substr(2);
    if (path.size() >= 2 && path[1] == L':') return LR"(\\?\)" + path;
    return path;
}

std::wstring windowsError(Language language, const wchar_t* operation,
                          const wchar_t* operationEnglish) {
    const DWORD code = GetLastError();
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    const bool english = language == Language::English;
    std::wstring message = english
        ? (operationEnglish ? operationEnglish : L"File operation")
        : (operation ? operation : L"文件操作");
    message += english
        ? L" (Windows error " + std::to_wstring(code) + L")"
        : L"（Windows 错误 " + std::to_wstring(code) + L"）";
    if (length && buffer) {
        message += (english ? L": " : L"：") + std::wstring(buffer, length);
        while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n')) {
            message.pop_back();
        }
    }
    if (buffer) LocalFree(buffer);
    return message;
}

uint16_t u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t u64(const uint8_t* p) {
    return static_cast<uint64_t>(u32(p)) | (static_cast<uint64_t>(u32(p + 4)) << 32);
}

class Reader {
public:
    Reader() = default;

    ~Reader() {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    }

    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    bool open(const std::wstring& path) {
        handle_ = CreateFileW(nativePath(path).c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(handle_, &size)) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
            return false;
        }
        size_ = static_cast<uint64_t>(size.QuadPart);
        return true;
    }

    uint64_t size() const { return size_; }

    bool read(uint64_t offset, void* output, size_t bytes) const {
        if (offset > size_ || bytes > size_ - offset) return false;
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(handle_, position, nullptr, FILE_BEGIN)) return false;
        uint8_t* out = static_cast<uint8_t*>(output);
        while (bytes) {
            const DWORD amount = static_cast<DWORD>(std::min<size_t>(bytes, 16 * 1024 * 1024));
            DWORD read = 0;
            if (!ReadFile(handle_, out, amount, &read, nullptr) || read == 0) return false;
            out += read;
            bytes -= read;
        }
        return true;
    }

    template <size_t N>
    bool read(uint64_t offset, std::array<uint8_t, N>& output) const {
        return read(offset, output.data(), output.size());
    }

    bool readVector(uint64_t offset, size_t bytes, std::vector<uint8_t>& output) const {
        output.resize(bytes);
        return bytes == 0 || read(offset, output.data(), bytes);
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    uint64_t size_ = 0;
};

std::wstring decodeName(const uint8_t* data, size_t size, bool utf8) {
    if (!size) return L"";
    const UINT codePage = utf8 ? CP_UTF8 : CP_ACP;
    const int length = MultiByteToWideChar(codePage, 0,
        reinterpret_cast<const char*>(data), static_cast<int>(size), nullptr, 0);
    if (length <= 0) return L"<invalid file name>";
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(codePage, 0, reinterpret_cast<const char*>(data),
                        static_cast<int>(size), result.data(), length);
    return result;
}

struct EocdCandidate {
    uint64_t offset = 0;
    std::array<uint8_t, 22> bytes{};
};

bool findEocdCandidates(const Reader& reader, std::vector<EocdCandidate>& candidates) {
    const uint64_t amount64 = std::min<uint64_t>(reader.size(), 2 * 1024 * 1024 + 65557);
    const size_t amount = static_cast<size_t>(amount64);
    const uint64_t base = reader.size() - amount64;
    std::vector<uint8_t> tail;
    if (!reader.readVector(base, amount, tail) || amount < 22) return false;
    for (size_t pos = amount - 22;; --pos) {
        if (u32(tail.data() + pos) == EOCD_SIG) {
            const uint16_t commentLength = u16(tail.data() + pos + 20);
            if (base + pos + 22ULL + commentLength <= reader.size()) {
                EocdCandidate candidate;
                candidate.offset = base + pos;
                std::copy_n(tail.data() + pos, 22, candidate.bytes.data());
                candidates.push_back(candidate);
            }
        }
        if (pos == 0) break;
    }
    return true;
}

bool readZip64Directory(const Reader& reader, uint64_t eocdOffset,
                        uint64_t& centralSize, uint64_t& centralOffset,
                        uint32_t& entries, uint64_t& zip64EocdOffset) {
    if (eocdOffset < 20 + 56) return false;
    const uint64_t locatorOffset = eocdOffset - 20;
    std::array<uint8_t, 20> locator{};
    if (!reader.read(locatorOffset, locator) || u32(locator.data()) != ZIP64_LOCATOR_SIG) {
        return false;
    }
    const uint64_t scanAmount64 = std::min<uint64_t>(locatorOffset, 1024 * 1024 + 56);
    const size_t scanAmount = static_cast<size_t>(scanAmount64);
    const uint64_t base = locatorOffset - scanAmount64;
    std::vector<uint8_t> area;
    if (!reader.readVector(base, scanAmount, area) || scanAmount < 56) return false;
    for (size_t pos = scanAmount - 56;; --pos) {
        if (u32(area.data() + pos) == ZIP64_EOCD_SIG) {
            const uint64_t recordSize = u64(area.data() + pos + 4);
            if (recordSize >= 44 && recordSize <= locatorOffset - (base + pos) &&
                base + pos + 12 + recordSize == locatorOffset) {
                const uint64_t total64 = u64(area.data() + pos + 32);
                if (total64 > 0 && total64 <= 10000000) {
                    centralSize = u64(area.data() + pos + 40);
                    centralOffset = u64(area.data() + pos + 48);
                    entries = static_cast<uint32_t>(total64);
                    zip64EocdOffset = base + pos;
                    return true;
                }
            }
        }
        if (pos == 0) break;
    }
    return false;
}

uint64_t zip64ExtraValue(const uint8_t* extra, size_t extraSize,
                         uint32_t uncompressed32, uint32_t compressed32,
                         uint32_t local32) {
    size_t pos = 0;
    while (pos + 4 <= extraSize) {
        const uint16_t id = u16(extra + pos);
        const uint16_t size = u16(extra + pos + 2);
        pos += 4;
        if (pos + size > extraSize) break;
        if (id == 0x0001) {
            size_t field = pos;
            const size_t end = pos + size;
            if (uncompressed32 == 0xFFFFFFFF) {
                if (field + 8 > end) return 0;
                field += 8;
            }
            if (compressed32 == 0xFFFFFFFF) {
                if (field + 8 > end) return 0;
                field += 8;
            }
            if (local32 == 0xFFFFFFFF) {
                if (field + 8 > end) return 0;
                return u64(extra + field);
            }
            return 0;
        }
        pos += size;
    }
    return 0;
}

bool buildLayout(const Reader& reader, const EocdCandidate& candidate, Layout& layout) {
    const auto& e = candidate.bytes;
    const uint16_t disk = u16(e.data() + 4);
    const uint16_t centralDisk = u16(e.data() + 6);
    const uint32_t entries32 = u16(e.data() + 10);
    uint64_t centralSize = u32(e.data() + 12);
    uint64_t centralOffset = u32(e.data() + 16);
    uint32_t entries = entries32;
    bool zip64 = false;
    uint64_t zip64Eocd = 0;

    if (disk != 0 || centralDisk != 0 || entries == 0) return false;
    const bool hasZip64 = readZip64Directory(reader, candidate.offset, centralSize,
                                              centralOffset, entries, zip64Eocd);
    if (hasZip64) {
        zip64 = true;
    } else if (entries32 == 0xFFFF || centralSize == 0xFFFFFFFFULL ||
               centralOffset == 0xFFFFFFFFULL) {
        return false;
    }
    if (entries == 0 || centralSize == 0 || centralSize > reader.size()) return false;

    const uint64_t directoryEnd = zip64 ? zip64Eocd : candidate.offset;
    if (centralSize > directoryEnd) return false;
    const uint64_t actualCentral = directoryEnd - centralSize;
    if (centralOffset > actualCentral) return false;
    const uint64_t prefix = actualCentral - centralOffset;
    std::array<uint8_t, 4> signature{};
    if (!reader.read(actualCentral, signature) || u32(signature.data()) != CENTRAL_SIG) {
        return false;
    }

    Layout next;
    next.fileSize = reader.size();
    next.archiveStart = prefix;
    next.eocdOffset = candidate.offset;
    next.archiveEnd = candidate.offset + 22 + u16(e.data() + 20);
    next.centralOffset = actualCentral;
    next.centralSize = centralSize;
    next.entryCount = entries;
    next.zip64 = zip64;

    uint64_t cursor = actualCentral;
    for (uint32_t index = 0; index < entries; ++index) {
        if (cursor + 46 > actualCentral + centralSize) return false;
        std::array<uint8_t, 46> fixed{};
        if (!reader.read(cursor, fixed) || u32(fixed.data()) != CENTRAL_SIG) return false;
        const uint16_t flags = u16(fixed.data() + 8);
        const uint16_t nameLength = u16(fixed.data() + 28);
        const uint16_t extraLength = u16(fixed.data() + 30);
        const uint16_t commentLength = u16(fixed.data() + 32);
        const uint32_t compressed32 = u32(fixed.data() + 20);
        const uint32_t uncompressed32 = u32(fixed.data() + 24);
        const uint32_t local32 = u32(fixed.data() + 42);
        const uint64_t recordSize = 46ULL + nameLength + extraLength + commentLength;
        if (cursor + recordSize > actualCentral + centralSize) return false;
        std::vector<uint8_t> variable;
        if (!reader.readVector(cursor + 46,
                               static_cast<size_t>(nameLength + extraLength), variable)) {
            return false;
        }

        uint64_t localOffset = local32;
        if (local32 == 0xFFFFFFFF) {
            localOffset = zip64ExtraValue(variable.data() + nameLength, extraLength,
                                          uncompressed32, compressed32, local32);
        }
        if (localOffset > reader.size() || prefix > reader.size() - localOffset) return false;
        const uint64_t actualLocal = prefix + localOffset;
        std::array<uint8_t, 4> local{};
        if (!reader.read(actualLocal, local) || u32(local.data()) != LOCAL_SIG) return false;

        EntryInfo info;
        info.name = decodeName(variable.data(), nameLength, (flags & 0x800) != 0);
        info.localOffset = actualLocal;
        next.entries.push_back(std::move(info));
        cursor += recordSize;
    }
    if (next.entries.empty() || cursor != actualCentral + centralSize) return false;
    layout = std::move(next);
    return true;
}

// ---------- 格式识别 ----------
// 伪装样本的形态是「一段结构完整的视频容器 + 真正的压缩包」。
// 所以先走视频容器的盒子结构拿到断点，再在断点处匹配压缩包签名，
// 能自校验的（7z 的头部 CRC、RAR5 的逐块 CRC）再逐段验一遍。

uint32_t u32be(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

uint64_t u64be(const uint8_t* p) {
    return (static_cast<uint64_t>(u32be(p)) << 32) | static_cast<uint64_t>(u32be(p + 4));
}

uint32_t crc32Of(const uint8_t* data, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

size_t varintLength(const uint8_t* data, size_t available, uint64_t& value) {
    uint64_t result = 0;
    size_t shift = 0;
    for (size_t index = 0; index < available && index < 10; ++index) {
        const uint8_t byte = data[index];
        result |= static_cast<uint64_t>(byte & 0x7Fu) << shift;
        if (!(byte & 0x80u)) {
            value = result;
            return index + 1;
        }
        shift += 7;
    }
    return 0;
}

bool readProbe(const Reader& reader, uint64_t offset, std::array<uint8_t, 32>& output,
               size_t& amount) {
    amount = 0;
    if (offset >= reader.size()) return false;
    amount = static_cast<size_t>(std::min<uint64_t>(output.size(), reader.size() - offset));
    return reader.read(offset, output.data(), amount);
}

struct IsoWalk {
    bool sawFtyp = false;
    bool clean = false;
    uint64_t end = 0;
};

IsoWalk walkIsoBmff(const Reader& reader) {
    IsoWalk walk;
    const uint64_t size = reader.size();
    uint64_t position = 0;
    for (int guard = 0; guard < 100000 && position + 8 <= size; ++guard) {
        std::array<uint8_t, 32> head{};
        size_t amount = 0;
        if (!readProbe(reader, position, head, amount) || amount < 8) break;
        const uint32_t shortSize = u32be(head.data());
        const uint8_t* type = head.data() + 4;
        bool printable = true;
        for (int index = 0; index < 4; ++index) {
            if (type[index] < 0x20 || type[index] > 0x7E) {
                printable = false;
                break;
            }
        }
        if (!printable) break;
        uint64_t boxSize = shortSize;
        uint64_t headerSize = 8;
        if (shortSize == 1) {
            if (amount < 16) break;
            boxSize = u64be(head.data() + 8);
            headerSize = 16;
        } else if (shortSize == 0) {
            boxSize = size - position;
        }
        if (boxSize < headerSize || boxSize > size - position) break;
        if (std::memcmp(type, "ftyp", 4) == 0) walk.sawFtyp = true;
        position += boxSize;
    }
    walk.end = position;
    walk.clean = walk.sawFtyp && position == size;
    return walk;
}

const uint8_t kRar5Magic[] = {0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x01, 0x00};
const uint8_t kRar4Magic[] = {0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x00};
const uint8_t kSevenZipMagic[] = {0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C};
const uint8_t kZipLocalMagic[] = {0x50, 0x4B, 0x03, 0x04};
const uint8_t kCabMagic[] = {0x4D, 0x53, 0x43, 0x46};
const uint8_t kGzipMagic[] = {0x1F, 0x8B};
const uint8_t kBzip2Magic[] = {0x42, 0x5A, 0x68};
const uint8_t kXzMagic[] = {0xFD, 0x37, 0x7A, 0x58, 0x5A, 0x00};
const uint8_t kZstdMagic[] = {0x28, 0xB5, 0x2F, 0xFD};
const uint8_t kAceMagic[] = {0x2A, 0x2A, 0x41, 0x43, 0x45, 0x2A, 0x2A};
const uint8_t kArjMagic[] = {0x60, 0xEA};
const uint8_t kFlvMagic[] = {0x46, 0x4C, 0x56};
const uint8_t kAsfMagic[] = {0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11};
const uint8_t kMkvMagic[] = {0x1A, 0x45, 0xDF, 0xA3};
const uint8_t kMpegPsMagic[] = {0x00, 0x00, 0x01, 0xBA};

bool matches(const uint8_t* data, size_t amount, const uint8_t* magic, size_t length) {
    return amount >= length && std::memcmp(data, magic, length) == 0;
}

Format signatureFormat(const Reader& reader, uint64_t offset) {
    std::array<uint8_t, 32> head{};
    size_t amount = 0;
    if (!readProbe(reader, offset, head, amount) || amount < 2) return Format::Unknown;
    const uint8_t* data = head.data();
    if (matches(data, amount, kRar5Magic, sizeof(kRar5Magic))) return Format::Rar5;
    if (matches(data, amount, kRar4Magic, sizeof(kRar4Magic))) return Format::Rar4;
    if (matches(data, amount, kSevenZipMagic, sizeof(kSevenZipMagic))) return Format::SevenZip;
    if (matches(data, amount, kZipLocalMagic, sizeof(kZipLocalMagic))) return Format::Zip;
    if (matches(data, amount, kCabMagic, sizeof(kCabMagic))) return Format::Cab;
    if (matches(data, amount, kGzipMagic, sizeof(kGzipMagic))) return Format::GZip;
    if (matches(data, amount, kBzip2Magic, sizeof(kBzip2Magic))) return Format::BZip2;
    if (matches(data, amount, kXzMagic, sizeof(kXzMagic))) return Format::Xz;
    if (matches(data, amount, kZstdMagic, sizeof(kZstdMagic))) return Format::Zstd;
    if (matches(data, amount, kAceMagic, sizeof(kAceMagic))) return Format::Ace;
    if (matches(data, amount, kArjMagic, sizeof(kArjMagic))) return Format::Arj;
    if (matches(data, amount, kMkvMagic, sizeof(kMkvMagic))) return Format::Matroska;
    if (matches(data, amount, kFlvMagic, sizeof(kFlvMagic))) return Format::Flv;
    if (matches(data, amount, kAsfMagic, sizeof(kAsfMagic))) return Format::Asf;
    if (matches(data, amount, kMpegPsMagic, sizeof(kMpegPsMagic))) return Format::MpegPs;
    if (amount >= 12 && std::memcmp(data, "RIFF", 4) == 0 &&
        std::memcmp(data + 8, "AVI ", 4) == 0) {
        return Format::Avi;
    }
    if (amount >= 5 && data[2] == '-' && data[3] == 'l' && data[4] == 'h') return Format::Lzh;
    std::array<uint8_t, 6> iso{};
    if (offset <= reader.size() && reader.size() - offset > 0x8001 + 5 &&
        reader.read(offset + 0x8001, iso) && std::memcmp(iso.data(), "CD001", 5) == 0) {
        return Format::Iso;
    }
    std::array<uint8_t, 8> tar{};
    if (offset <= reader.size() && reader.size() - offset > 257 + 8 &&
        reader.read(offset + 257, tar) && std::memcmp(tar.data(), "ustar", 5) == 0) {
        return Format::Tar;
    }
    return Format::Unknown;
}

bool looksLikeMpegTs(const Reader& reader) {
    if (reader.size() < 3 * 188 + 1) return false;
    for (int packet = 0; packet < 3; ++packet) {
        uint8_t byte = 0;
        if (!reader.read(static_cast<uint64_t>(packet) * 188, &byte, 1) || byte != 0x47) {
            return false;
        }
    }
    return true;
}

bool verifySevenZip(const Reader& reader, uint64_t start, uint64_t& end) {
    std::array<uint8_t, 32> head{};
    if (start + 32 > reader.size() || !reader.read(start, head)) return false;
    if (!matches(head.data(), head.size(), kSevenZipMagic, sizeof(kSevenZipMagic))) return false;
    if (u32(head.data() + 8) != crc32Of(head.data() + 12, 20)) return false;
    const uint64_t nextOffset = u64(head.data() + 12);
    const uint64_t nextSize = u64(head.data() + 20);
    const uint64_t base = start + 32;
    if (nextSize == 0 || nextSize > 8ull * 1024 * 1024) return false;
    if (nextOffset > reader.size() - base || nextSize > reader.size() - base - nextOffset) {
        return false;
    }
    std::vector<uint8_t> next;
    if (!reader.readVector(base + nextOffset, static_cast<size_t>(nextSize), next)) return false;
    if (crc32Of(next.data(), next.size()) != u32(head.data() + 28)) return false;
    end = base + nextOffset + nextSize;
    return true;
}

bool verifyRar5(const Reader& reader, uint64_t start, uint64_t& end) {
    uint64_t position = start + 8;
    for (int guard = 0; guard < 2000000; ++guard) {
        if (position + 4 > reader.size()) return false;
        std::array<uint8_t, 4> crcBytes{};
        if (!reader.read(position, crcBytes)) return false;
        const uint32_t headerCrc = u32(crcBytes.data());
        std::array<uint8_t, 32> probe{};
        size_t probeAmount = 0;
        if (!readProbe(reader, position + 4, probe, probeAmount) || probeAmount < 2) return false;
        uint64_t headSize = 0;
        const size_t headSizeLength = varintLength(probe.data(), probeAmount, headSize);
        if (!headSizeLength || headSize < 2 || headSize > 2 * 1024 * 1024) return false;
        const uint64_t headerStart = position + 4;
        const uint64_t headerLength = headSizeLength + headSize;
        if (headerLength > reader.size() - headerStart) return false;
        std::vector<uint8_t> header;
        if (!reader.readVector(headerStart, static_cast<size_t>(headerLength), header)) return false;
        if (crc32Of(header.data(), header.size()) != headerCrc) return false;

        size_t cursor = headSizeLength;
        uint64_t headType = 0;
        uint64_t flags = 0;
        uint64_t dataSize = 0;
        size_t used = varintLength(header.data() + cursor, header.size() - cursor, headType);
        if (!used) return false;
        cursor += used;
        used = varintLength(header.data() + cursor, header.size() - cursor, flags);
        if (!used) return false;
        cursor += used;
        if (flags & 0x0001u) {
            uint64_t extraSize = 0;
            used = varintLength(header.data() + cursor, header.size() - cursor, extraSize);
            if (!used) return false;
            cursor += used;
        }
        if (flags & 0x0002u) {
            used = varintLength(header.data() + cursor, header.size() - cursor, dataSize);
            if (!used) return false;
            cursor += used;
        }
        const uint64_t dataStart = headerStart + headerLength;
        if (dataSize > reader.size() - dataStart) return false;
        position = dataStart + dataSize;
        if (headType == 5) {
            end = position;
            return true;
        }
        if (headType < 1 || headType > 5) return false;
    }
    return false;
}

bool verifyArchive(const Reader& reader, Format format, uint64_t start, uint64_t& end) {
    end = 0;
    if (format == Format::SevenZip) return verifySevenZip(reader, start, end);
    if (format == Format::Rar5) return verifyRar5(reader, start, end);
    return false;
}

void detectFormat(const Reader& reader, Layout& layout) {
    layout.fileSize = reader.size();
    const IsoWalk walk = walkIsoBmff(reader);
    if (walk.sawFtyp) {
        layout.format = Format::Mp4;
        layout.archiveStart = walk.end;
        layout.archiveEnd = walk.clean ? reader.size() : 0;
        layout.verified = walk.clean;
        if (!walk.clean && walk.end > 0 && walk.end < reader.size()) {
            const Format tail = signatureFormat(reader, walk.end);
            if (formatIsArchive(tail)) {
                layout.format = tail;
                layout.archiveStart = walk.end;
                layout.archiveEnd = 0;
                layout.verified = verifyArchive(reader, tail, walk.end, layout.archiveEnd);
            }
        }
        return;
    }

    Format format = signatureFormat(reader, 0);
    if (format == Format::Unknown && looksLikeMpegTs(reader)) format = Format::MpegTs;
    layout.format = format;
    layout.archiveStart = 0;
    layout.archiveEnd = 0;
    layout.verified = false;
    if (formatIsArchive(format)) {
        layout.verified = verifyArchive(reader, format, 0, layout.archiveEnd);
    }
}

std::wstring fileName(const std::wstring& path) {
    const size_t separator = path.find_last_of(L"/\x5c");
    return separator == std::wstring::npos ? path : path.substr(separator + 1);
}

std::wstring parentDirectory(const std::wstring& path) {
    const size_t separator = path.find_last_of(L"/\x5c");
    if (separator == std::wstring::npos) return L".";
    if (separator == 2 && path.size() >= 3 && path[1] == L':') return path.substr(0, 3);
    if (separator == 0) return path.substr(0, 1);
    return path.substr(0, separator);
}

std::wstring joinPath(const std::wstring& directory, const std::wstring& name) {
    if (directory.empty() || directory == L".") return directory.empty() ? name : L".\x5c" + name;
    if (directory.back() == L'\x5c' || directory.back() == L'/') return directory + name;
    return directory + L"\x5c" + name;
}

bool pathExists(const std::wstring& path) {
    return GetFileAttributesW(nativePath(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool directoryExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(nativePath(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool ensureDirectory(const std::wstring& path) {
    if (path.empty() || directoryExists(path)) return true;
    std::wstring clean = path;
    while (clean.size() > 3 && (clean.back() == L'\x5c' || clean.back() == L'/')) clean.pop_back();
    const std::wstring parent = parentDirectory(clean);
    if (parent != clean && !parent.empty() && !directoryExists(parent) && !ensureDirectory(parent)) {
        return false;
    }
    if (CreateDirectoryW(nativePath(clean).c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS && directoryExists(clean);
}

std::wstring uniqueOutputPath(const std::wstring& source, const std::wstring& directory,
                              const std::wstring& extension, Language language) {
    std::wstring stem = fileName(source);
    const size_t dot = stem.find_last_of(L'.');
    if (dot != std::wstring::npos && dot != 0) stem.erase(dot);
    const std::wstring suffix = language == Language::English ? L"_repaired" : L"_已修复";
    const std::wstring base = joinPath(directory, stem + suffix + extension);
    if (!pathExists(base)) return base;
    for (unsigned index = 2; index < 10000; ++index) {
        const std::wstring candidate = joinPath(
            directory, stem + suffix + L"_" + std::to_wstring(index) + extension);
        if (!pathExists(candidate)) return candidate;
    }
    return L"";
}

bool truncateFile(const std::wstring& path, uint64_t size) {
    HANDLE file = CreateFileW(nativePath(path).c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(size);
    const bool ok = SetFilePointerEx(file, position, nullptr, FILE_BEGIN) && SetEndOfFile(file);
    const DWORD error = GetLastError();
    CloseHandle(file);
    SetLastError(error);
    return ok;
}

Result fail(Result result, const std::wstring& target, const std::wstring& message,
            Language language = Language::Chinese) {
    if (!target.empty()) DeleteFileW(nativePath(target).c_str());
    result.status = Status::Failure;
    if (!message.empty()) {
        result.message = message;
    } else {
        result.message = language == Language::English ? L"Conversion failed" : L"转换失败";
    }
    return result;
}

} // namespace

bool Layout::repairable() const {
    if (format == Format::Zip) return verified;
    if (format == Format::SevenZip || format == Format::Rar5) return verified;
    return false;
}

const wchar_t* formatName(Format format) {
    switch (format) {
        case Format::Zip: return L"ZIP";
        case Format::Rar4: return L"RAR";
        case Format::Rar5: return L"RAR5";
        case Format::SevenZip: return L"7Z";
        case Format::Cab: return L"CAB";
        case Format::Iso: return L"ISO";
        case Format::GZip: return L"GZIP";
        case Format::BZip2: return L"BZIP2";
        case Format::Xz: return L"XZ";
        case Format::Zstd: return L"ZSTD";
        case Format::Tar: return L"TAR";
        case Format::Ace: return L"ACE";
        case Format::Arj: return L"ARJ";
        case Format::Lzh: return L"LZH";
        case Format::Mp4: return L"MP4";
        case Format::Matroska: return L"MKV";
        case Format::Avi: return L"AVI";
        case Format::Flv: return L"FLV";
        case Format::Asf: return L"WMV";
        case Format::MpegPs: return L"MPEG";
        case Format::MpegTs: return L"TS";
        default: return L"未知";
    }
}

const wchar_t* formatExtension(Format format) {
    switch (format) {
        case Format::Zip: return L".zip";
        case Format::SevenZip: return L".7z";
        case Format::Rar4:
        case Format::Rar5: return L".rar";
        default: return L".zip";
    }
}

bool formatIsArchive(Format format) {
    switch (format) {
        case Format::Zip:
        case Format::Rar4:
        case Format::Rar5:
        case Format::SevenZip:
        case Format::Cab:
        case Format::Iso:
        case Format::GZip:
        case Format::BZip2:
        case Format::Xz:
        case Format::Zstd:
        case Format::Tar:
        case Format::Ace:
        case Format::Arj:
        case Format::Lzh:
            return true;
        default:
            return false;
    }
}

bool formatIsVideo(Format format) {
    switch (format) {
        case Format::Mp4:
        case Format::Matroska:
        case Format::Avi:
        case Format::Flv:
        case Format::Asf:
        case Format::MpegPs:
        case Format::MpegTs:
            return true;
        default:
            return false;
    }
}

std::wstring describeLayout(const Layout& layout, Language language) {
    const bool english = language == Language::English;
    if (formatIsVideo(layout.format)) {
        if (layout.format == Format::Mp4 && layout.verified) {
            return english
                ? L"This is an MP4 video, not an archive (" + formatSize(layout.fileSize) + L")"
                : L"这是 MP4 视频，并非压缩包（" + formatSize(layout.fileSize) + L"）";
        }
        if (layout.format == Format::Mp4 && layout.archiveStart) {
            const std::wstring end = formatSize(layout.archiveStart);
            return english
                ? L"This is an MP4 video, not an archive (container ends at " + end + L")"
                : L"这是 MP4 视频，并非压缩包（容器结构在 " + end + L" 处结束）";
        }
        return english
            ? L"This is a " + std::wstring(formatName(layout.format)) + L" video, not an archive"
            : L"这是 " + std::wstring(formatName(layout.format)) + L" 视频，并非压缩包";
    }
    if (layout.format == Format::Unknown) {
        return english ? L"No archive structure recognized" : L"未识别出压缩包结构";
    }
    std::wstring text = english
        ? std::wstring(formatName(layout.format)) + L" archive detected"
        : L"检测到 " + std::wstring(formatName(layout.format)) + L" 压缩包";
    if (layout.archiveStart) {
        text += english ? L" · prefix " + formatSize(layout.archiveStart)
                        : L" · 前缀 " + formatSize(layout.archiveStart);
    }
    if (layout.format == Format::Zip) {
        if (english) {
            text += L" · " + std::to_wstring(layout.entryCount) +
                    (layout.entryCount == 1 ? L" entry" : L" entries");
        } else {
            text += L" · " + std::to_wstring(layout.entryCount) + L" 个条目";
        }
        if (layout.zip64) text += L" · ZIP64";
    }
    if (!layout.verified) {
        text += layout.isZip()
            ? (english ? L" · central directory not verifiable" : L" · 中央目录无法验证")
            : (english ? L" · structure not verified" : L" · 结构未验证");
    }
    return text;
}

Layout inspect(const std::wstring& path, Language language) {
    Layout layout;
    Reader reader;
    if (!reader.open(path)) {
        layout.error = language == Language::English
            ? L"Unable to open the source file or read its size"
            : L"无法打开源文件或读取文件大小";
        return layout;
    }
    layout.fileSize = reader.size();
    std::vector<EocdCandidate> candidates;
    if (findEocdCandidates(reader, candidates)) {
        for (const auto& candidate : candidates) {
            if (buildLayout(reader, candidate, layout)) {
                layout.format = Format::Zip;
                layout.verified = true;
                return layout;
            }
        }
    }
    layout = Layout{};
    layout.fileSize = reader.size();
    detectFormat(reader, layout);
    if (layout.format == Format::Unknown) {
        layout.error = language == Language::English
            ? L"No verifiable ZIP / RAR / 7z structure recognized"
            : L"未识别出可验证的 ZIP/RAR/7z 结构";
    }
    return layout;
}

std::wstring makeOutputPath(const std::wstring& source, const std::wstring& outputDirectory,
                            Format format, Language language) {
    const std::wstring directory = outputDirectory.empty() ? parentDirectory(source) : outputDirectory;
    if (!ensureDirectory(directory)) return L"";
    return uniqueOutputPath(source, directory, formatExtension(format), language);
}

Result convert(const std::wstring& path, const Options& options,
               void* context, ProgressCallback progress, CancelCallback cancelled) {
    const Language language = options.language;
    const bool english = language == Language::English;
    Result result;
    result.input = path;
    result.layout = inspect(path, language);
    const Layout source = result.layout;
    if (!source.error.empty()) {
        const std::wstring message = source.error;
        return fail(std::move(result), L"", message, language);
    }
    if (formatIsVideo(source.format)) {
        result.status = Status::NoConversion;
        result.message = describeLayout(source, language);
        return result;
    }
    if (!source.repairable()) {
        const std::wstring note = english ? L"; this version does not rewrite it"
                                          : L"；本版不会自动改写";
        return fail(std::move(result), L"", describeLayout(source, language) + note, language);
    }
    if (source.archiveStart == 0) {
        result.status = Status::NoConversion;
        if (source.trailingBytes()) {
            result.message = english
                ? L"Structure is fine; no copy made (trailing data does not affect use)"
                : L"结构正常；未复制文件（尾部残留不影响使用）";
        } else {
            result.message = english ? L"Structure is fine; no conversion needed"
                                     : L"结构正常，无需转换";
        }
        return result;
    }

    const std::wstring target = makeOutputPath(path, options.outputDirectory, source.format,
                                               language);
    if (target.empty()) {
        const std::wstring message = english
            ? L"Unable to create the output directory or allocate an output file name"
            : L"无法创建输出目录或分配输出文件名";
        return fail(std::move(result), L"", message, language);
    }
    result.output = target;
    HANDLE writer = CreateFileW(nativePath(target).c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (writer == INVALID_HANDLE_VALUE) {
        const std::wstring message = windowsError(language, L"创建输出文件失败",
                                                  L"Failed to create the output file");
        return fail(std::move(result), L"", message, language);
    }

    Reader reader;
    if (!reader.open(path)) {
        CloseHandle(writer);
        return fail(std::move(result), target,
                    english ? L"Unable to reopen the source file" : L"无法重新打开源文件",
                    language);
    }
    std::vector<uint8_t> buffer(COPY_BUFFER_SIZE);
    const uint64_t total = reader.size() - source.archiveStart;
    uint64_t remaining = total;
    uint64_t copied = 0;
    uint64_t readOffset = source.archiveStart;
    bool failed = false;
    while (remaining) {
        if (cancelled && cancelled(context)) {
            result.status = Status::Cancelled;
            result.message = english ? L"Cancelled; the temporary output was removed"
                                     : L"已取消，临时输出已删除";
            failed = true;
            break;
        }
        const size_t amount = static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size()));
        if (!reader.read(readOffset, buffer.data(), amount)) {
            result.message = english ? L"Failed to read the source file" : L"读取源文件失败";
            failed = true;
            break;
        }
        const uint8_t* out = buffer.data();
        size_t left = amount;
        while (left) {
            DWORD written = 0;
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(left, 16 * 1024 * 1024));
            if (!WriteFile(writer, out, chunk, &written, nullptr) || written == 0) {
                result.message = windowsError(language, L"写入输出文件失败",
                                              L"Failed to write the output file");
                failed = true;
                break;
            }
            out += written;
            left -= written;
        }
        if (failed) break;
        readOffset += amount;
        remaining -= amount;
        copied += amount;
        if (progress) progress(context, copied, total);
    }
    if (!CloseHandle(writer) && !failed) {
        result.message = windowsError(language, L"关闭输出文件失败",
                                      L"Failed to close the output file");
        failed = true;
    }
    if (failed) {
        DeleteFileW(nativePath(target).c_str());
        if (result.status != Status::Cancelled) result.status = Status::Failure;
        if (result.message.empty()) {
            result.message = english ? L"Conversion failed" : L"转换失败";
        }
        return result;
    }

    Layout repaired = inspect(target, language);
    if (!repaired.error.empty()) return fail(std::move(result), target, repaired.error, language);
    bool valid = repaired.format == source.format && repaired.archiveStart == 0 &&
                 repaired.verified;
    if (valid && source.isZip()) {
        valid = repaired.entryCount == source.entryCount &&
                repaired.entries.size() == source.entries.size();
        for (size_t i = 0; valid && i < repaired.entries.size(); ++i) {
            valid = repaired.entries[i].name == source.entries[i].name;
        }
    }
    if (!valid) {
        return fail(std::move(result), target,
                    english ? L"Structure verification failed after repair"
                            : L"修复后结构验证失败",
                    language);
    }

    uint64_t trimmed = 0;
    if (options.trimTail && repaired.trailingBytes()) {
        trimmed = repaired.trailingBytes();
        if (!truncateFile(target, repaired.archiveEnd)) {
            const std::wstring message = windowsError(language, L"清理尾部失败",
                                                      L"Failed to trim trailing data");
            return fail(std::move(result), target, message, language);
        }
        repaired = inspect(target, language);
        if (!repaired.error.empty() || repaired.trailingBytes() != 0 || !repaired.verified) {
            return fail(std::move(result), target,
                        english ? L"Verification failed after trimming trailing data"
                                : L"尾部清理后验证失败",
                        language);
        }
    }

    const uint64_t sourcePrefix = source.archiveStart;
    result.layout = std::move(repaired);
    result.status = Status::Success;
    if (english) {
        result.message = L"Converted; stripped a " + std::to_wstring(sourcePrefix) +
                         L"-byte prefix";
        if (trimmed) {
            result.message += L", trimmed " + std::to_wstring(trimmed) +
                              L" bytes of trailing data";
        }
    } else {
        result.message = L"转换完成；切除前缀 " + std::to_wstring(sourcePrefix) + L" 字节";
        if (trimmed) result.message += L"，清理尾部 " + std::to_wstring(trimmed) + L" 字节";
    }
    return result;
}
std::wstring formatSize(uint64_t bytes) {
    if (bytes < 1024) return std::to_wstring(bytes) + L" B";
    const wchar_t* units[] = {L"KB", L"MB", L"GB", L"TB"};
    uint64_t divisor = 1024;
    int unit = 0;
    while (unit < 3 && bytes / divisor >= 1024) {
        divisor *= 1024;
        ++unit;
    }
    uint64_t whole = bytes / divisor;
    uint64_t decimals = ((bytes % divisor) * 100 + divisor / 2) / divisor;
    if (decimals == 100) {
        ++whole;
        decimals = 0;
    }
    std::wstring text = std::to_wstring(whole) + L".";
    if (decimals < 10) text += L"0";
    text += std::to_wstring(decimals) + L" " + units[unit];
    return text;
}

std::wstring statusText(Status status, Language language) {
    const bool english = language == Language::English;
    switch (status) {
        case Status::Success: return english ? L"Done" : L"完成";
        case Status::NoConversion: return english ? L"Nothing to convert" : L"无需转换";
        case Status::Cancelled: return english ? L"Cancelled" : L"已取消";
        default: return english ? L"Failed" : L"失败";
    }
}

} // namespace ziprepair
