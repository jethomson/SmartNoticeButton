import os
import json

Import('env')

def generate_file_list(source, target, env):
  data_dir = env.subst('$PROJECT_DATA_DIR')
  FILES_ROOT = "files"
  FILES_ROOT_PATH = os.path.join(data_dir, FILES_ROOT)
  output_file = os.path.join(FILES_ROOT_PATH, 'file_list.json')

  if os.path.exists(output_file):
    os.remove(output_file)
    
  tree = {}
  for dirpath, dirnames, filenames in os.walk(FILES_ROOT_PATH):
    rel_path = os.path.relpath(dirpath, FILES_ROOT_PATH)
    if rel_path == '.':
      continue
    key = rel_path.replace(os.sep, '/')
    tree[key] = filenames

  LITTLEFS_ROOT = "/"+FILES_ROOT
  structure = {LITTLEFS_ROOT: tree}
  for key in structure[LITTLEFS_ROOT]:
    structure[LITTLEFS_ROOT][key].sort()

  with open(output_file, 'w') as f:
    json.dump(structure, f, indent=2)

env.AddPreAction("$BUILD_DIR/${ESP32_FS_IMAGE_NAME}.bin", generate_file_list)
