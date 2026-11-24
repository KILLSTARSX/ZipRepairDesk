from pathlib import Path
import hashlib
import shutil
import struct
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT / "dist" / "ZipRepairDesk-CLI.exe"
WORK = ROOT / "tests" / ".work"
FIXTURES = WORK / "fixtures"
OUTPUT = WORK / "输出目录"


def run(*args, expected=0):
    result = subprocess.run([str(CLI), *map(str, args)], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
    print(result.stdout, end="")
    if result.returncode != expected:
        raise AssertionError(f"exit={result.returncode}, expected={expected}")
    return result.stdout


def box(kind, payload):
    return struct.pack(">I4s", len(payload) + 8, kind) + payload


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    if not CLI.is_file():
        raise SystemExit(f"缺少 {CLI}，先运行 powershell -ExecutionPolicy Bypass -File build.ps1")
    if WORK.exists():
        shutil.rmtree(WORK)
    FIXTURES.mkdir(parents=True)
    OUTPUT.mkdir(parents=True)
    base = FIXTURES / "基础.zip"
    with zipfile.ZipFile(base, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("目录/你好.txt", "批量修复测试\n" * 1000)
        archive.writestr("data.bin", bytes(range(256)) * 100)
        archive.comment = b"native-core-test"
    original = base.read_bytes()
    mp4 = box(b"ftyp", b"isom\x00\x00\x02\x00isomiso2") + box(b"free", b"preview" * 31)
    tail = b"RANDOM-TRAILER" * 17
    prefixed = FIXTURES / "伪装样本.mp4"
    prefixed.write_bytes(mp4 + original)
    # 复现真实样本：标准 EOCD 的目录字段仍是 32 位值，但前面存在
    # ZIP64 EOCD + locator；中央目录必须止于 ZIP64 EOCD。
    eocd = original.rfind(b"PK\x05\x06")
    fields = struct.unpack_from("<IHHHHIIH", original, eocd)
    entries, central_size, central_offset = fields[4], fields[5], fields[6]
    zip64_record = struct.pack("<IQHHIIQQQQ", 0x06064B50, 44, 45, 45, 0, 0,
                               entries, entries, central_size, central_offset)
    zip64_locator = struct.pack("<IIQI", 0x07064B50, 0, eocd, 1)
    zip64_original = original[:eocd] + zip64_record + zip64_locator + original[eocd:]
    zip64_sample = FIXTURES / "ZIP64样本.mp4"
    zip64_sample.write_bytes(mp4 + zip64_original)
    trailed = FIXTURES / "带尾部.zip"
    trailed.write_bytes(mp4 + original + tail)
    healthy = FIXTURES / "正常.zip"
    healthy.write_bytes(original)
    invalid = FIXTURES / "无效.bin"
    invalid.write_bytes(b"not an archive" * 100)
    before = {path: digest(path) for path in (base, prefixed, zip64_sample, trailed, healthy, invalid)}

    text = run("inspect", prefixed, zip64_sample, trailed, healthy)
    assert f"ZIP 起点: {len(mp4)}" in text
    assert "条目: 目录/你好.txt" in text
    assert f"尾部: {len(tail)}" in text
    run("convert", "--output", OUTPUT, prefixed, zip64_sample, trailed)
    outputs = sorted(OUTPUT.glob("*.zip"))
    assert len(outputs) == 3, outputs
    for output in outputs:
        with zipfile.ZipFile(output) as archive:
            assert archive.testzip() is None
            assert archive.read("目录/你好.txt") == ("批量修复测试\n" * 1000).encode()
            assert archive.read("data.bin") == bytes(range(256)) * 100
            assert archive.comment == b"native-core-test"
    assert (OUTPUT / "带尾部_已修复.zip").stat().st_size == len(original)
    run("convert", "--output", OUTPUT, prefixed)
    assert (OUTPUT / "伪装样本_已修复_2.zip").is_file()
    healthy_output = run("convert", "--output", OUTPUT, healthy)
    assert "无需转换" in healthy_output
    assert not (OUTPUT / "正常_已修复.zip").exists()
    run("inspect", invalid, expected=1)
    after = {path: digest(path) for path in before}
    assert before == after, "源文件发生变化"
    print("PASS: inspect / batch convert / ZIP64 / trim / unique name / CRC / source immutability")
    print(f"测试目录: {WORK}")


if __name__ == "__main__":
    main()
