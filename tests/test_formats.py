import hashlib
import os
import shutil
import struct
import subprocess
import sys
import zipfile
import zlib

ROOT = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(ROOT)
CLI = os.path.join(PROJECT, "dist", "ZipRepairDesk-CLI.exe")
SEVEN = r"C:\Program Files\7-Zip\7z.exe"
WORK = os.path.join(ROOT, ".fmt-work")
FIX = os.path.join(WORK, "fixtures")
OUT = os.path.join(WORK, "output")

failures = []

def check(condition, label):
    print(("PASS " if condition else "FAIL ") + label)
    if not condition:
        failures.append(label)

def run(args):
    proc = subprocess.run(args, capture_output=True)
    text = (proc.stdout + proc.stderr).decode("utf-8", "replace")
    return proc.returncode, text

def sha(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()

def box(kind, payload):
    return struct.pack(">I", 8 + len(payload)) + kind + payload

def mp4_prefix(extra=b""):
    body = box(b"ftyp", b"isom\x00\x00\x02\x00isomiso2") + box(b"mdat", b"\x00" * 64 + extra)
    return body

def vint(value):
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)

def rar5_blob(name=b"a.txt", data=b"hello rar5\n"):
    def header(kind, flags, body):
        common = vint(kind) + vint(flags) + body
        size = vint(len(common))
        crc = zlib.crc32(size + common) & 0xFFFFFFFF
        return struct.pack("<I", crc) + size + common
    main = header(1, 0, vint(0))
    body = (vint(len(data))
            + vint(0x0004)
            + vint(len(data))
            + vint(0x20)
            + struct.pack("<I", zlib.crc32(data) & 0xFFFFFFFF)
            + vint(0) + vint(2) + vint(len(name)) + name)
    return b"Rar!\x1a\x07\x01\x00" + main + header(2, 0x0002, body) + data + header(5, 0, vint(0))

def prepare():
    if os.path.isdir(WORK):
        shutil.rmtree(WORK)
    os.makedirs(FIX)
    os.makedirs(OUT)

def write(name, blob):
    path = os.path.join(FIX, name)
    with open(path, "wb") as handle:
        handle.write(blob)
    return path

def make_7z():
    source = os.path.join(WORK, "src")
    os.makedirs(source)
    with open(os.path.join(source, "one.txt"), "w", encoding="utf-8") as handle:
        handle.write("first payload\n")
    os.makedirs(os.path.join(source, "sub"))
    with open(os.path.join(source, "sub", "two.bin"), "wb") as handle:
        handle.write(bytes(range(256)) * 200)
    archive = os.path.join(WORK, "plain.7z")
    code, text = run([SEVEN, "a", "-t7z", "-mx1", archive, os.path.join(source, "*")])
    check(code == 0 and os.path.exists(archive), "7z fixture created")
    return archive

def main():
    if not os.path.exists(CLI):
        print("FAIL missing CLI: " + CLI)
        return 1
    if not os.path.exists(SEVEN):
        print("FAIL missing 7-Zip: " + SEVEN)
        return 1
    prepare()
    prefix = mp4_prefix()
    seven = make_7z()

    # 1. 纯 MP4：应提示不是压缩包
    plain_mp4 = write("纯视频.mp4", mp4_prefix())
    code, text = run([CLI, "inspect", plain_mp4])
    check(code == 0 and "格式: MP4" in text and "并非压缩包" in text, "inspect pure mp4 reports video")
    code, text = run([CLI, "convert", "--output", OUT, plain_mp4])
    check(code == 0 and "无需转换" in text and "并非压缩包" in text, "convert pure mp4 refuses with message")
    check(os.listdir(OUT) == [], "pure mp4 produced no output")

    # 2. MP4 前缀 + 7Z
    with open(seven, "rb") as handle:
        seven_bytes = handle.read()
    seven_case = write("伪装7z.mp4", prefix + seven_bytes)
    code, text = run([CLI, "inspect", seven_case])
    check(code == 0 and "格式: 7Z" in text and ("起点: %d" % len(prefix)) in text, "inspect mp4+7z finds start")
    code, text = run([CLI, "convert", "--output", OUT, seven_case])
    produced = [os.path.join(OUT, name) for name in os.listdir(OUT)]
    check(code == 0 and len(produced) == 1 and produced[0].endswith(".7z"), "convert mp4+7z writes .7z")
    if produced:
        check(sha(produced[0]) == sha(seven), "mp4+7z slice is byte exact")
        code, text = run([SEVEN, "t", produced[0]])
        check(code == 0 and "Everything is Ok" in text, "repaired 7z passes 7-Zip test")
        os.remove(produced[0])

    # 3. MP4 前缀 + RAR5
    rar_bytes = rar5_blob()
    rar_case = write("伪装rar5.mp4", prefix + rar_bytes)
    code, text = run([CLI, "inspect", rar_case])
    check(code == 0 and "格式: RAR5" in text and "结构未验证" not in text, "inspect mp4+rar5 verifies chain")
    code, text = run([CLI, "convert", "--output", OUT, rar_case])
    produced = [os.path.join(OUT, name) for name in os.listdir(OUT)]
    check(code == 0 and len(produced) == 1 and produced[0].endswith(".rar"), "convert mp4+rar5 writes .rar")
    if produced:
        check(sha(produced[0]) == hashlib.sha256(rar_bytes).hexdigest(), "mp4+rar5 slice is byte exact")
        code, text = run([SEVEN, "t", produced[0]])
        check(code == 0 and "Everything is Ok" in text, "repaired rar5 passes 7-Zip test")
        os.remove(produced[0])

    # 4. RAR4 只识别不修复
    rar4 = write("旧rar.mp4", b"Rar!\x1a\x07\x00" + b"\x00" * 200)
    code, text = run([CLI, "inspect", rar4])
    check(code == 0 and "格式: RAR" in text and "结构未验证" in text, "inspect rar4 identifies only")
    code, text = run([CLI, "convert", "--output", OUT, rar4])
    check(code == 1 and "本版不会自动改写" in text, "convert rar4 reports unsupported")
    check(os.listdir(OUT) == [], "rar4 produced no output")

    # 5. 其他视频签名
    mkv = write("伪装mkv.mp4", prefix + b"\x1a\x45\xdf\xa3" + b"\x00" * 100)
    code, text = run([CLI, "inspect", mkv])
    check(code == 0 and "格式: MP4" in text and "并非压缩包" in text, "inspect mp4+mkv still reports mp4")
    flv = write("纯flv.mp4", b"FLV\x01\x05\x00\x00\x00\x09" + b"\x00" * 200)
    code, text = run([CLI, "inspect", flv])
    check(code == 0 and "格式: FLV" in text and "并非压缩包" in text, "inspect bare flv reports video")

    # 6. 普通 ZIP 无需转换
    plain_zip = os.path.join(FIX, "普通.zip")
    with zipfile.ZipFile(plain_zip, "w") as archive:
        archive.writestr("a.txt", "hello")
    code, text = run([CLI, "inspect", plain_zip])
    check(code == 0 and "格式: ZIP" in text and "ZIP 起点: 0" in text, "inspect plain zip stays zip")
    code, text = run([CLI, "convert", "--output", OUT, plain_zip])
    check(code == 0 and "无需转换" in text and os.listdir(OUT) == [], "plain zip needs no conversion")

    if failures:
        print("FAILURES: " + "; ".join(failures))
        return 1
    print("PASS: format detection / 7z slice / rar5 slice / rar4 report / video message / plain zip")
    return 0

if __name__ == "__main__":
    sys.exit(main())
