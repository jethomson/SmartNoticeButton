Import("env")
env.Execute("$PYTHONEXE -m pip install minify-html")

from pathlib import Path
import shutil
import os
import minify_html
import gzip

def minify(source, target, env):
  #html_files = [Path("www", "compositor.htm")] # run on single file
  html_files = Path("www").glob("*.htm")
  if html_files:
    dest = Path("www_minified")
    os.makedirs(dest, exist_ok=True)
    
    for html_filepath in html_files:
      html_filename = html_filepath.name
      html = html_filepath.read_text()
      html_minified = minify_html.minify(html, minify_js=True, minify_css=True, keep_input_type_text_attr=True)
      with gzip.open(dest.joinpath(f"{html_filename}.gz"), "wb") as fgz:
        fgz.write(html_minified.encode("utf-8"))

  json_files = Path("www").glob("*.json")
  if json_files:
    dest = Path("www_minified")
    os.makedirs(dest, exist_ok=True)
    
    for json_filepath in json_files:
      json_filename = json_filepath.name
      json = json_filepath.read_text()
      json_minified = minify_html.minify(json, minify_js=True, minify_css=True, keep_input_type_text_attr=True)
      with gzip.open(dest.joinpath(f"{json_filename}.gz"), "wb") as fgz:
        fgz.write(json_minified.encode("utf-8"))

  js_files = Path("www", "js").glob("*.js")
  if js_files:
    dest = Path("www_minified", "js")
    os.makedirs(dest, exist_ok=True)

    for js_filepath in js_files:
      js_filename = js_filepath.name
      shutil.copy(js_filepath, dest.joinpath(js_filename))

env.AddPostAction("buildfs", minify)

