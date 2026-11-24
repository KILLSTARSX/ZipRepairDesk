#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ziprepair {

struct EntryInfo {
    std::wstring name;
    uint64_t localOffset = 0;
};

// 导入文件的实际格式：压缩包与视频分开判断，避免把「视频+压缩包」的伪装样本
// 当成纯视频，也避免把纯视频当成坏掉的压缩包。
enum class Format {
    Unknown = 0,
    Zip,
    Rar4,
    Rar5,
    SevenZip,
    Cab,
    Iso,
    GZip,
    BZip2,
    Xz,
    Zstd,
    Tar,
    Ace,
    Arj,
    Lzh,
    Mp4,
    Matroska,
    Avi,
    Flv,
    Asf,
    MpegPs,
    MpegTs
};

struct Layout {
    uint64_t fileSize = 0;
    uint64_t archiveStart = 0;  // 压缩包或视频结构的起始偏移
    uint64_t eocdOffset = 0;    // 仅 ZIP
    uint64_t archiveEnd = 0;    // 自校验确认的结构末尾，0 表示未知
    uint64_t centralOffset = 0; // 仅 ZIP
    uint64_t centralSize = 0;   // 仅 ZIP
    uint32_t entryCount = 0;    // 仅 ZIP
    std::vector<EntryInfo> entries;
    bool zip64 = false;
    Format format = Format::Unknown;
    bool verified = false;      // 结构自校验是否通过
    std::wstring error;

    uint64_t prefixBytes() const { return archiveStart; }
    uint64_t trailingBytes() const {
        return archiveEnd && archiveEnd <= fileSize ? fileSize - archiveEnd : 0;
    }
    bool isZip() const { return format == Format::Zip; }
    bool repairable() const;
};

enum class Language { Chinese = 0, English = 1 };

struct Options {
    std::wstring outputDirectory;
    bool trimTail = true;
    Language language = Language::Chinese;
};

enum class Status { Success, NoConversion, Cancelled, Failure };

struct Result {
    Status status = Status::Failure;
    std::wstring input;
    std::wstring output;
    std::wstring message;
    Layout layout;
};

using ProgressCallback = void (*)(void* context, uint64_t copied, uint64_t total);
using CancelCallback = bool (*)(void* context);

Layout inspect(const std::wstring& path, Language language = Language::Chinese);
Result convert(const std::wstring& path,
               const Options& options,
               void* context = nullptr,
               ProgressCallback progress = nullptr,
               CancelCallback cancelled = nullptr);
std::wstring makeOutputPath(const std::wstring& source,
                            const std::wstring& outputDirectory,
                            Format format = Format::Zip,
                            Language language = Language::Chinese);
std::wstring formatSize(uint64_t bytes);
std::wstring statusText(Status status, Language language = Language::Chinese);

const wchar_t* formatName(Format format);
const wchar_t* formatExtension(Format format);
bool formatIsArchive(Format format);
bool formatIsVideo(Format format);
std::wstring describeLayout(const Layout& layout, Language language = Language::Chinese);

} // namespace ziprepair
