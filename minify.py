Import('env')
env.Execute('$PYTHONEXE -m pip install minify-html')

from pathlib import Path
import shutil
import os
import minify_html
import gzip


def minify(source, target, env):

  SRC_DIR = 'www'
  DEST_DIR = 'www_minified'

  #upload_cmd += f' {env.get('ESP32_APP_OFFSET')} {env.get('PROGNAME')}.bin'
  #print(env.gvars())

  build_flags = env.ParseFlags(env['BUILD_FLAGS'])
  flags_with_value_list = [build_flag for build_flag in build_flags.get('CPPDEFINES') if type(build_flag) == list]
  defines = {k: v for (k, v) in flags_with_value_list}

  if 'DEBUG_LOG' in defines and defines['DEBUG_LOG'] == '1':
    html_files = Path('www').glob('*.htm')
  else:
    # if DEBUG_LOG is not set to 1 in platformio.ini (-DDEBUG_LOG=1) then save some space by not including debug_log.htm
    html_files = [path for path in Path(SRC_DIR).glob('*.htm') if 'debug_log.htm' not in path.name]

  #html_files = [Path('www', 'config.htm')] # run on single file for testing
  if html_files:
    dest = Path(DEST_DIR)
    shutil.rmtree(dest, ignore_errors=True)
    #os.makedirs(dest, exist_ok=True)
    place_holder = Path(f'./{DEST_DIR}/README.md')
    place_holder.parent.mkdir(exist_ok=True, parents=True)
    with open(place_holder, 'w') as f:
        f.write('This folder is intentionally left empty in the repository. The webpages output by minify.py during the build are created here.')
    
    for html_filepath in html_files:
      html_filename = html_filepath.name
      html = html_filepath.read_text()
      html_minified = minify_html.minify(html, minify_js=True, minify_css=True, keep_input_type_text_attr=True)
      with gzip.open(dest.joinpath(f'{html_filename}.gz'), 'wb') as fgz:
        fgz.write(html_minified.encode('utf-8'))

  json_files = Path(SRC_DIR).glob("*.json")
  if json_files:
    dest = Path(DEST_DIR)
    
    for json_filepath in json_files:
      json_filename = json_filepath.name
      json = json_filepath.read_text()
      json_minified = minify_html.minify(json, minify_js=True, minify_css=True, keep_input_type_text_attr=True)
      with gzip.open(dest.joinpath(f"{json_filename}.gz"), "wb") as fgz:
        fgz.write(json_minified.encode("utf-8"))

  js_files = Path(SRC_DIR, 'js').glob('*.js')
  if js_files:
    dest = Path(DEST_DIR, 'js')
    os.makedirs(dest, exist_ok=True)

    for js_filepath in js_files:
      js_filename = js_filepath.name
      shutil.copy(js_filepath, dest.joinpath(js_filename))

#env.AddPreAction('buildfs', minify) # this actually calls minify after buildfs has finished
env.AddPreAction("$BUILD_DIR/${ESP32_FS_IMAGE_NAME}.bin", minify)

