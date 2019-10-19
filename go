x=poc98
make $x && adb push $x /data/local/tmp && adb shell chmod 755 /data/local/tmp/$x # && adb shell /data/local/tmp/$x