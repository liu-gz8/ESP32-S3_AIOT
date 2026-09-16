"""ESP-SR 集成脚本(PlatformIO extra_scripts)

做两件事:
1. 构建结束后运行 esp-sr 自带的 movemodel.py,把 sdkconfig 里选中的模型
   打包成 srmodels.bin(PlatformIO 不会执行 IDF 的自定义 ALL 目标)
2. 把 srmodels.bin 注册到 FLASH_EXTRA_IMAGES,烧录时写到 partitions.csv 中
   name=model 的分区

启用方式(platformio.ini):
    extra_scripts = tools/pio_esp_sr.py
"""

import os
import subprocess
import sys

Import("env")

try:
    from SCons.Script import COMMAND_LINE_TARGETS
except ImportError:
    COMMAND_LINE_TARGETS = []

project_dir = env.subst("$PROJECT_DIR")
build_dir = env.subst("$BUILD_DIR")
env_name = env.subst("$PIOENV")

partitions_csv = os.path.join(project_dir, "partitions.csv")
sdkconfig = os.path.join(project_dir, "sdkconfig.%s" % env_name)
sr_component = os.path.join(project_dir, "managed_components", "espressif__esp-sr")
movemodel_py = os.path.join(sr_component, "model", "movemodel.py")
srmodels_bin = os.path.join(build_dir, "srmodels", "srmodels.bin")


def model_partition_offset():
    """从 partitions.csv 读取 name=model 的分区起始地址"""
    if not os.path.isfile(partitions_csv):
        return None
    with open(partitions_csv, "r", encoding="utf-8") as fp:
        for line in fp:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            fields = [field.strip() for field in line.split(",")]
            if len(fields) >= 4 and fields[0] == "model":
                return fields[3]
    return None


def pack_models(target, source, env):
    if not os.path.isfile(movemodel_py):
        print("[esp-sr] movemodel.py not found, skip packing models")
        return
    subprocess.check_call(
        [
            sys.executable,
            movemodel_py,
            "-d1",
            sdkconfig,
            "-d2",
            sr_component,
            "-d3",
            build_dir,
        ]
    )


env.AddPostAction("buildprog", pack_models)

offset = model_partition_offset()

# PlatformIO 在加载本脚本之前就已经拼好了 UPLOADERFLAGS,所以这里直接追加
# (uploadfs 会重写 UPLOADERFLAGS,那种情况下不要插进去)
if offset and "uploadfs" not in COMMAND_LINE_TARGETS:
    env.Append(UPLOADERFLAGS=[offset, srmodels_bin])
    print("[esp-sr] flash image registered: %s @ %s" % (srmodels_bin, offset))
elif not offset:
    print("[esp-sr] no 'model' partition in partitions.csv, models won't be flashed")
