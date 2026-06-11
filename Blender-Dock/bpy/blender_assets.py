import bpy
import sys

OUTPUT_FILE: str = "assets_result.txt"


def output(msg: str = "") -> None:
    with open(OUTPUT_FILE, "a", encoding="utf-8") as f:
        f.write(msg + "\n")


def parse_args() -> list:
    if "--" in sys.argv:
        return sys.argv[sys.argv.index("--") + 1 :]
    return []


def list_asset_libs() -> None:
    output("当前资产库：")
    libs = bpy.context.preferences.filepaths.asset_libraries
    if not libs:
        output("  (无)")
        return
    for i, lib in enumerate(libs):
        output(f"[{i}] {lib.name} -> {lib.path}")


def add_asset_lib(name: str, path: str) -> None:
    # 先检查是否已存在
    libs = bpy.context.preferences.filepaths.asset_libraries
    if any(lib.name == name for lib in libs):
        output(f"资源库 '{name}' 已存在")
        return

    # 方法1：使用操作符添加，并指定 directory 参数（如果支持）
    # 注意：Blender 3.0 的 asset_library_add 可能没有 directory 参数
    # 使用简单添加，然后修改属性
    try:
        bpy.ops.preferences.asset_library_add()
        # 新添加的库在末尾
        new_lib = libs[-1]
        new_lib.name = name
        new_lib.path = path
        # 强制保存用户设置
        bpy.ops.wm.save_userpref()
        output(f"添加资源库：{name} -> {path}")
    except Exception as e:
        output(f"添加失败：{e}")


def remove_asset_lib(identifier: str) -> None:
    libs = bpy.context.preferences.filepaths.asset_libraries
    target_index = None
    if identifier.isdigit():
        idx = int(identifier)
        if 0 <= idx < len(libs):
            target_index = idx
    else:
        for i, lib in enumerate(libs):
            if lib.name == identifier:
                target_index = i
                break
    if target_index is None:
        output(f"未找到资源库: {identifier}")
        return
    try:
        bpy.ops.preferences.asset_library_remove(index=target_index)
        bpy.ops.wm.save_userpref()
        output(f"删除了资源库（索引 {target_index}）")
    except Exception as e:
        output(f"删除失败: {e}")


def modify_asset_lib(identifier: str, new_name=None, new_path=None) -> None:
    libs = bpy.context.preferences.filepaths.asset_libraries
    target = None
    if identifier.isdigit():
        idx = int(identifier)
        if 0 <= idx < len(libs):
            target = libs[idx]
    else:
        for lib in libs:
            if lib.name == identifier:
                target = lib
                break
    if target is None:
        output(f"未找到资源库: {identifier}")
        return
    if new_name:
        target.name = new_name
    if new_path:
        target.path = new_path
    bpy.ops.wm.save_userpref()
    output(f"修改后: {target.name} -> {target.path}")


def main() -> None:
    open(OUTPUT_FILE, "w").close()
    bpy.app.debug = False
    args = parse_args()
    if not args:
        output("用法:")
        output("  list")
        output("  add <名称> <路径>")
        output("  remove <名称|索引>")
        output("  modify <名称|索引> --name <新名称> --path <新路径>")
        return

    cmd = args[0]
    if cmd == "list":
        list_asset_libs()
        bpy.context.preferences.is_dirty = False
    elif cmd == "add" and len(args) >= 3:
        add_asset_lib(args[1], args[2])
    elif cmd == "remove" and len(args) >= 2:
        remove_asset_lib(args[1])
    elif cmd == "modify" and len(args) >= 2:
        name_or_index = args[1]
        new_name = None
        new_path = None
        if "--name" in args:
            new_name = args[args.index("--name") + 1]
        if "--path" in args:
            new_path = args[args.index("--path") + 1]
        modify_asset_lib(name_or_index, new_name, new_path)
    else:
        output("未知命令或参数不足")


if __name__ == "__main__":
    main()
