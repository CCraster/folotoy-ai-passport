"""约束实机曾溢出的设置调用链；SDK 调用深度继续通过实机高水位验证。"""
from pathlib import Path
import sys


def main():
    directory = Path(sys.argv[1]) / "esp-idf" / "main"
    frames = {}
    for report in directory.rglob("*.su"):
        for line in report.read_text().splitlines():
            location, size, _ = line.split("\t")
            name = location.rsplit(":", 1)[-1].split(".", 1)[0]
            frames[name] = max(frames.get(name, 0), int(size))
    # Wi-Fi 开关不应为音乐配置或整个扫描列表预留栈空间。
    budgets = {"app_main": 2304, "handle_button": 384, "save_editor": 128,
               "open_editor": 192, "network_set_enabled": 256, "start_saved": 512,
               "photo_decoder_info": 128}
    for name, limit in budgets.items():
        if name not in frames:
            raise SystemExit(f"Missing stack report for {name}; run a full firmware build")
        if frames[name] > limit:
            raise SystemExit(f"Stack budget exceeded: {name} {frames[name]} > {limit} bytes")
    print("Stack budgets passed: " + ", ".join(f"{name}={frames[name]}" for name in budgets))


if __name__ == "__main__":
    main()
