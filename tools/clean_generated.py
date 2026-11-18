#!/usr/bin/env python3
"""清理默认构建产物，保留实际配置并移出历史实板测量证据。"""
from pathlib import Path
import shutil


def main():
    root = Path(__file__).resolve().parents[1]
    build = root / "build" / "default"
    if not build.exists():
        print("没有默认构建产物需要清理")
        return
    if build.is_symlink() or not build.resolve().is_relative_to(root):
        raise RuntimeError("拒绝清理工作区以外的构建目录")
    # 配置、编译数据库和配置头文件仍用于 IDE 阅读，不属于二进制产物。
    retained = ("sdkconfig", "compile_commands.json", "config/sdkconfig.h")
    saved = {
        name: (build / name).read_bytes()
        for name in retained
        if (build / name).is_file()
    }
    evidence = root / "reference" / "validation"
    patterns = ("*.csv", "*.csv.metadata.json", "*validation.json",
                "*metrics.json")
    sources = sorted(
        {p for pattern in patterns for p in build.glob(pattern) if p.is_file()})
    if sources:
        evidence.mkdir(parents=True, exist_ok=True)
    for source in sources:
        target = evidence / source.name
        # 已有证据不得覆盖；同名不同内容保留为带编号的独立文件。
        number = 1
        while target.exists() and target.read_bytes() != source.read_bytes():
            target = evidence / f"{source.stem}-{number}{source.suffix}"
            number += 1
        if not target.exists():
            shutil.copy2(source, target)
    shutil.rmtree(build)
    for name, contents in saved.items():
        target = build / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(contents)
    # Python 缓存也不是阅读源码所需文件，测试结束后可一并清理。
    for directory in (root / "tools", root / "tests"):
        for cache in directory.rglob("__pycache__"):
            shutil.rmtree(cache)
    print(f"已清理编译产物，保留实际配置、IDE 索引及 {len(sources)} 份历史测量文件")


if __name__ == "__main__":
    main()
