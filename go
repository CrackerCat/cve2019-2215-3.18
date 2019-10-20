x=poc98
make $x$1 && adb push $x$1 /data/local/tmp && adb shell chmod 755 /data/local/tmp/$x$1 #&& adb shell /data/local/tmp/$x$1