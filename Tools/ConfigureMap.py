import unreal


MAP_PATH = "/Game/Untitled"

unreal.EditorLevelLibrary.load_level(MAP_PATH)
world = unreal.EditorLevelLibrary.get_editor_world()
world_settings = world.get_world_settings()

world_settings.set_editor_property("enable_world_bounds_checks", False)

if not unreal.EditorLevelLibrary.save_current_level():
    raise RuntimeError(f"Failed to save {MAP_PATH}")

unreal.log("AirCombatSim: disabled world bounds checks for Cesium and saved /Game/Untitled.")
