#include "repair_core.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

void print(const std::wstring& text) {
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written = 0;
    const std::wstring line = text + L"\n";
    if (WriteConsoleW(output, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr)) return;
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()),
                                          nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(std::max(bytes, 0)), '\0');
    if (bytes > 0) WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()),
                                      utf8.data(), bytes, nullptr, nullptr);
    WriteFile(output, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
}

void usage() {
    print(L"用法：");
    print(L"  ZipRepairDesk-CLI.exe inspect <文件> [更多文件…]");
    print(L"  ZipRepairDesk-CLI.exe convert [--output 目录] [--keep-tail] <文件> [更多文件…]");
}

void showProgress(void* context, uint64_t copied, uint64_t total) {
    int* previous = static_cast<int*>(context);
    const int percent = total ? static_cast<int>(copied * 100 / total) : 100;
    if (percent >= *previous + 10 || percent == 100) {
        *previous = percent;
        print(L"进度: " + std::to_wstring(percent) + L"%");
    }
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        usage();
        return 2;
    }
    const std::wstring command = argv[1];
    ziprepair::Options options;
    std::vector<std::wstring> paths;
    for (int i = 2; i < argc; ++i) {
        const std::wstring argument = argv[i];
        if (command == L"convert" && argument == L"--output" && i + 1 < argc) {
            options.outputDirectory = argv[++i];
        } else if (command == L"convert" && argument == L"--keep-tail") {
            options.trimTail = false;
        } else {
            paths.push_back(argument);
        }
    }
    if (paths.empty() || (command != L"inspect" && command != L"convert")) {
        usage();
        return 2;
    }
    int failures = 0;
    for (const auto& path : paths) {
        if (command == L"inspect") {
            const ziprepair::Layout layout = ziprepair::inspect(path);
            if (!layout.error.empty()) {
                print(L"失败: " + layout.error);
                ++failures;
                continue;
            }
            print(L"文件: " + path);
            print(L"大小: " + std::to_wstring(layout.fileSize));
            print(L"格式: " + std::wstring(ziprepair::formatName(layout.format)));
            print(L"说明: " + ziprepair::describeLayout(layout));
            if (!layout.isZip()) {
                print(L"起点: " + std::to_wstring(layout.archiveStart));
                print(L"结构末尾: " + std::to_wstring(layout.archiveEnd));
                continue;
            }
            print(L"ZIP 起点: " + std::to_wstring(layout.archiveStart));
            print(L"EOCD: " + std::to_wstring(layout.eocdOffset));
            print(L"归档末尾: " + std::to_wstring(layout.archiveEnd));
            print(L"中央目录: " + std::to_wstring(layout.centralOffset));
            print(L"目录大小: " + std::to_wstring(layout.centralSize));
            print(L"条目数: " + std::to_wstring(layout.entryCount));
            print(L"ZIP64: " + std::wstring(layout.zip64 ? L"是" : L"否"));
            print(L"尾部: " + std::to_wstring(layout.trailingBytes()));
            for (const auto& entry : layout.entries) print(L"条目: " + entry.name);
        } else {
            print(L"开始: " + path);
            int previous = -1;
            const ziprepair::Result result = ziprepair::convert(
                path, options, &previous, &showProgress, nullptr);
            print(ziprepair::statusText(result.status) + L": " + result.message);
            if (!result.output.empty()) print(L"输出: " + result.output);
            if (result.status == ziprepair::Status::Failure ||
                result.status == ziprepair::Status::Cancelled) ++failures;
        }
    }
    return failures ? 1 : 0;
}
