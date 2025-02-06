#!/bin/bash

# Include helper functions
THIS_SCRIPT_PATH=$(dirname $(readlink -f "$0"))
source "$THIS_SCRIPT_PATH"/../../cmake_targets/tools/build_helper

# Set environment variables (OPENAIR_HOME, ...)
set_openair_env

echo $OPENAIR_DIR
NB_UE=0
CLEAN=false

for arg in "$@"; do
  case $arg in
    --nb-ue)
      NB_UE="$2"
      shift 2
      ;;
    -c)
      CLEAN=true
      shift
      ;;
    *)
      ;;
  esac
done

cleanup() {
  echo "Cleaning up namespaces and stopping processes..."

  # Stop all namespaces for UEs
  if [[ $NB_UE -gt 0 ]]; then
    for ((i=1; i<=NB_UE; i++)); do
      echo "Deleting namespace for UE $i..."
      $OPENAIR_DIR/tools/scripts/multi-ue.sh -d "$i"
    done
  fi

  # Kill gNB and UE processes
  echo "Stopping gNB and UE processes..."
  pkill -f nr-softmodem
  pkill -f nr-uesoftmodem

  echo "Cleanup complete."
}

if $CLEAN; then
  cleanup
  exit 0
fi

if [[ $NB_UE -le 0 ]]; then
  echo "Usage: $0 --nb-ue <number_of_ues> [-c]"
  exit 1
fi

run_command() {
  "$@" &
}

sudo mkdir -p $OPENAIR_DIR/logs
echo "Starting the gNB..."
cd $OPENAIR_DIR/cmake_targets/ran_build/build || exit
run_command sudo ./nr-softmodem -O ../../../targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb.sa.band77.fr1.273PRB.usrpx300.conf\
  --gNBs.[0].min_rxtxtime 6 --rfsim > $OPENAIR_DIR/logs/gNB_output.log 2>&1

sleep 5

echo "Launching $NB_UE UEs..."
for ((i=1; i<=NB_UE; i++)); do
  echo "Launching UE $i in its namespace..."

  if ! ip netns list | grep -q "ue$i"; then
    echo "Namespace ue$i does not exist. Creating it..."
    sudo $OPENAIR_DIR/tools/scripts/multi-ue.sh -c $i
  else
    echo "Namespace ue$i already exists."
  fi


  IMSI=$(printf "%015d" $((1010000000000 + i)))
  BROKER_IP="10.$((200 + i)).1.100"
  LOG_FILE="$OPENAIR_DIR/logs/UE_${i}.log"

  ip netns exec "ue$i" bash -c "
    cd $OPENAIR_DIR/cmake_targets/ran_build/build
sudo ./nr-uesoftmodem -r 273 --numerology 1 --band 77 -C 3949740000  --ssb 1492 \
--uicc0.imsi $IMSI --rfsim --device_id $i --brokerip \"$BROKER_IP\" \
    > \"$LOG_FILE\" 2>&1 &
  "
  sleep 7
  echo "Exiting namespace for UE $i."
done

echo "Displaying gNB output:"
tail -f $OPENAIR_DIR/logs/gNB_output.log


