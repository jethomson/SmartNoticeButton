from pathlib import Path
import shutil
import os

Import('env')


def dist(source, target, env):
  DEST_DIR = 'dist'
  dest = Path(DEST_DIR)
  os.makedirs(dest, exist_ok=True)
  
  build_dir = env.subst('$BUILD_DIR')

  bootloader = f'{build_dir}/bootloader.bin'
  partitions = f'{build_dir}/partitions.bin'
  boot_app0 = env.get('FLASH_EXTRA_IMAGES')[2][1]
  firmware = f'{build_dir}/firmware.bin'
  littlefs = f'{build_dir}/littlefs.bin'


  if os.path.exists(bootloader):
    shutil.copy(bootloader, dest)
  else:
    print(f'{bootloader} not found.')
    
  if os.path.exists(partitions):
    shutil.copy(partitions, dest)
  else:
    print(f'{partitions} not found.')

  if os.path.exists(boot_app0):
    shutil.copy(boot_app0, dest)
  else:
    print(f'{boot_app0} not found.')

  if os.path.exists(firmware):
    shutil.copy(firmware, dest)
  else:
    print(f'{firmware} not found.')
    
  if os.path.exists(littlefs):
    shutil.copy(littlefs, dest)
  else:
    print(f'{littlefs} not found.')


  with open(Path(dest, 'README.TXT'), 'w') as fr:
    print("""Standalone executables of esptool can be downloaded from https://github.com/espressif/esptool/releases

Versions of esptool for Linux, Mac, and Windows are already included in the esptool folder

Connect your device to the computer via a USB cable.

Linux: give execute permission to esptool/linux/esptool and upload_linux.sh, then run upload_linux.sh to program your device.
Mac: give execute permission to esptool/mac/esptool and upload_mac.sh, then run upload_mac.sh to program your device.
Windows: double click upload_win.bat to program your device.


The program may appear to hang after you see text similar to the snippet below.
It is still working. It takes about a minute for it to erase the flash.
After that you will start getting output again.
---snip--
Wrote 367232 bytes (214663 compressed) at 0x00010000 in 8.0 seconds (effective 366.3 kbit/s)...
Hash of data verified.
Erasing flash...
Compressed 16187392 bytes to 8865443...
---snip--
After it has finished writing everything (takes about 6 minutes).
The program will appear to hang again.
After about 30 seconds, it will output a message and then exit.

The device should restart automatically.

The device will create a WiFi access point with the SSID: PixelArt.
Connect to PixelArt.
Enter the SSID and password for your WiFi router.
Chose a hostname for multicast DNS (e.g. pixelart).
Enter the number of rows and columns for your LED matrix.
Click Save.
The device should restart and connect to your wireless network.
In your web browser go to the mDNS address you set (e.g. http://pixelart.local).
Have fun converting images to pixel art, adding effects, and making playlists!
""", file=fr)

  upload_cmd = ''
  merge_cmd = ''
  uf = env.subst(env.get('UPLOADERFLAGS'))
  cmd_parts = ['', '', '']
  i = 0
  pi = 0
  offsets_dec = []
  while i < len(uf):
    # do not include port argument
    # counting on port being autodiscovered esptool. port names vary between OSes.
    if uf[i] == '--port':
      pi += 1
      i += 2
      continue

    if uf[i].startswith('0x'):
      offsets_dec.append(int(uf[i], 16))

    if uf[i].startswith('--flash_mode'):
      pi += 1
    
    if uf[i].endswith('.bin'):
      cmd_parts[pi] += f' {os.path.basename(uf[i])}'
    else:
      cmd_parts[pi] += f' {uf[i]}'
    i += 1

  offsets_dec.append(int(env.get('ESP32_APP_OFFSET'), 16))
  offsets_dec.append(env.get('FS_START')) # these offsets can be used for creating manifest.json

  cmd_parts[2] += f' {env.get('ESP32_APP_OFFSET')} {env.get("PROGNAME")}.bin'
  cmd_parts[2] += f' {hex(env.get('FS_START'))} {env.get("ESP32_FS_IMAGE_NAME")}.bin'

  upload_cmd = cmd_parts[0] + cmd_parts[1] + cmd_parts[2]
  merged_firmware = 'merged_firmware.bin'
  merge_cmd = f'"$PYTHONEXE" "$OBJCOPY" {cmd_parts[0]} merge_bin -o {merged_firmware} {cmd_parts[2]}'
  # it is suggested that using the hex format will not overwrite the nvs data (WiFi credentials, etc.) but that does not actually work
  # https://github.com/espressif/esptool/issues/1075
  #merged_firmware = 'merged_firmware.hex'
  #merge_cmd = f'"$PYTHONEXE" "$OBJCOPY" {cmd_parts[0]} merge_bin --format hex -o {merged_firmware} {cmd_parts[2]}'

  linux_upload_cmd = '#!/bin/bash\n'
  linux_upload_cmd += r'./esptool/linux/esptool --no-stub' + upload_cmd
  mac_upload_cmd = '#!/bin/bash\n'
  mac_upload_cmd += r'./esptool/mac/esptool --no-stub' + upload_cmd  
  win_upload_cmd = r'.\esptool\win\esptool.exe --no-stub' + upload_cmd

  with open(Path(dest, 'upload_linux.sh'), 'w') as fl:
    print(linux_upload_cmd, file=fl, end='')
  with open(Path(dest, 'upload_mac.sh'), 'w') as fm:
    print(mac_upload_cmd, file=fm, end='')
  with open(Path(dest, 'upload_win.bat'), 'w') as fw:
    print(win_upload_cmd, file=fw, end='')


  os.chdir(dest)
  env.Execute(merge_cmd)
  #merge_cmd_no_fs = '"$PYTHONEXE" "$OBJCOPY" --chip esp32 merge_bin -o merged_firmware_no_fs.bin --flash_mode dio --flash_freq 40m --flash_size 4MB 0x1000 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 firmware.bin'
  #env.Execute(merge_cmd_no_fs)

  if os.path.exists(merged_firmware):
    shutil.copy(merged_firmware, '../webflash/')
  else:
    print(f'{merged_firmware} not found.')



#if '$BUILD_DIR/littlefs.bin' is used as the target then dist is called for both Build Filesystem Image and Upload Filesytem Image
# however Upload Filesytem Image does not set all of the same UPLOADERFLAGS that Build Filesystem Image, which crashes the python script
# therefore use 'buildfs' as the target
#env.AddPostAction('$BUILD_DIR/littlefs.bin', dist)
env.AddPostAction('buildfs', dist)

