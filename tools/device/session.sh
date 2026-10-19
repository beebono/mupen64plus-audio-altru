#!/bin/sh
# ON THE DEVICE (rg35xxsp or rgdsplus).  One measurement session of the audio plugin(s): idle check, clocks pinned and restored, the runs.
#   session.sh rg35|rgds SECS COND:GAME ...        GAME = jfg ppl swr ewj sm64, COND = the suffix of plug/mupen64plus-audio-<COND>.so
# Layout under $D (default /mnt/sdcard/n64-dev/altru, /storage/altru-dev): plug/ (audio plugins as above, mupen64plus-rdp-foir.so,
# mupen64plus-rsp-armlet.so, mupen64plus-input-mash.so from mashinput.c), roms/ (rgds only), cpu_perf.sh (rg35 only, resources/toolchains/rg35xxsp/scripts),
# sm64.st (optional save state), extra.<COND> (extra mupen64plus arguments for that plugin, e.g. --set Audio-Altru[VOLUME_DEFAULT]=10).
# Env: GEXTRA (extra arguments for all runs), MASH_FF (see mashinput.c), FOIR_FRAMESKIP, ALTRU_STATS is set per run.
# Every run has its own private HOME / XDG / configdir (cfg-<COND>) under $D. Results: $D/out/<tag>.csv (buffer trace), .csv.sum (counters), .log.
# rg35xxsp: the codec PCM is held by the UI, so the (silent) HDMI codec PCM is used (AUDIODEV=hw:2,0): hardware-paced, period 1024 as usual.
DEV=$1; SECS=$2; shift 2
if [ "$DEV" = rg35 ]; then
  D=${D:-/mnt/sdcard/n64-dev/altru}; S=/mnt/SDCARD/Emu/N64/mupen64plus; RD=/mnt/SDCARD/Roms/N64DEV
  export LD_LIBRARY_PATH="$S:/mnt/SDCARD/App/PyUI/dll-mali:/mnt/SDCARD/spruce/flip/lib:/usr/lib:/usr/lib/aarch64-linux-gnu"
  UI=$S/mupen64plus; CORE=$S/libmupen64plus.so.2; DATA=$S; RES="--resolution 640x480"
  export AUDIODEV=hw:2,0
else
  D=${D:-/storage/altru-dev}; RD=$D/roms
  . /etc/profile >/dev/null 2>&1
  export XDG_RUNTIME_DIR=/var/run/0-runtime-dir WAYLAND_DISPLAY=wayland-1 SDL_VIDEODRIVER=wayland
  UI=/usr/local/bin/mupen64plus; CORE=/usr/local/lib/libmupen64plus.so.2; DATA=/usr/local/share/mupen64plus; RES=""
fi
P=$D/plug; O=$D/out; cd $D || exit 1
if pgrep -x mupen64plus >/dev/null; then echo BUSY; exit 3; fi
echo "session start: $(date +%T) load $(cat /proc/loadavg)"
mkdir -p $O $D/home/cache; touch $D/marker
GOV=/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
if [ "$DEV" = rg35 ]; then sh $D/cpu_perf.sh on; else OLDGOV=$(cat $GOV); echo performance > $GOV; fi
cleanup() { pkill -x mupen64plus 2>/dev/null
  if [ "$DEV" = rg35 ]; then sh $D/cpu_perf.sh off; else echo $OLDGOV > $GOV; echo "governor restored: $(cat $GOV)"; fi; }
trap cleanup EXIT INT TERM
export HOME=$D/home XDG_CONFIG_HOME=$D/home XDG_DATA_HOME=$D/home XDG_CACHE_HOME=$D/home/cache
for R in "$@"; do
  COND=${R%%:*}; GK=${R#*:}
  case $GK in
    jfg) ROM="Jet Force Gemini.z64" ;; ppl) ROM="Pokemon Puzzle League.z64" ;; swr) ROM="Star Wars Episode I - Racer.z64" ;;
    ewj) ROM="Earthworm Jim 3D.z64" ;; sm64) ROM="Super Mario 64.z64" ;; *) echo "unknown $GK"; continue ;;
  esac
  ST=""; [ "$GK" = sm64 ] && [ -f $D/sm64.st ] && ST="--savestate $D/sm64.st"
  AUD=$P/mupen64plus-audio-$COND.so; TAG=$DEV.$COND.$GK.$(date +%H%M%S)
  mkdir -p $D/cfg-$COND; EXTRA=""; [ -f $D/extra.$COND ] && EXTRA=$(cat $D/extra.$COND)
  rm -f $O/$TAG.*
  echo "== $TAG $(date +%T) temp $(cat /sys/class/thermal/thermal_zone0/temp) freq $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq)"
  ALTRU_STATS=$O/$TAG.csv MASH_STOP=$SECS timeout -k 8 $((SECS+40)) $UI --corelib $CORE --datadir $DATA --configdir $D/cfg-$COND --plugindir $P \
    --gfx $P/mupen64plus-rdp-foir.so --rsp $P/mupen64plus-rsp-armlet.so --audio $AUD --input $P/mupen64plus-input-mash.so \
    --noosd --fullscreen $RES --set "Core[SaveStatePath]=$D/home" $EXTRA $GEXTRA $ST "$RD/$ROM" > $O/$TAG.log 2>&1
  echo "exit=$?  $(cat $O/$TAG.csv.sum 2>/dev/null | tr '\n' ' ')"
  sleep 2
done
echo "outside-writes check:"; find /mnt/SDCARD/Emu /mnt/SDCARD/Saves /mnt/SDCARD/App/PyUI /root /usr/local -xdev -newer $D/marker -not -path "$D/*" 2>/dev/null | head
