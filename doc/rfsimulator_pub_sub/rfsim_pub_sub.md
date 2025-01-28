
# **Overview :**

The traditional implementation of the RF simulator has certain limitations, particularly with regard to sidelink and mobility features. This is due to its client-server architecture, which relied on raw sockets. To overcome these constraints, we transitioned to a publisher-subscriber paradigm using the ZMQ library. This new approach enhances scalability, enabling support for a larger number of UEs and cells.
# Architecture :
![rfsim_pub_sub_architecture]()
# Build :
## From build oai 

It's possible to build the rfsimulator device in its traditional implementation, whether by itself or with the OAI gNb or OAI UE

```
./build_oai --gNB --nrUE -w SIMU
```

to build the publisher-subscriber version you need to add "--zmq" option, 
```
./build_oai --gNb --nrUE --zmq
```
Alternatively:
```
./build_oai -w SIMU --zmq
```
# Usage :

Similar to the client-server rfsimulator, you can run the UEs and gNB. However, in the publisher-subscriber version, you must launch the broker first.
## Launch the broker :

```
cd openairinterface5g/cmake_targets/ran_build/build
./broker
```
## Launch the gNb : 

```
sudo ./nr-softmodem -O ../../../targets/PROJECTS/GENERIC-NR-5GC/CONF/gnb.sa.band78.fr1.106PRB.usrpb210.conf --gNBs.[0].min_rxtxtime 6 --rfsim
```

## Launch multiple UEs :

refer to https://gitlab.eurecom.fr/oai/openairinterface5g/-/blob/develop/doc/NR_SA_Tutorial_OAI_multi_UE.md for detailed instructions.

For each UE specify its device-id as well as the accessible broker-ip from its namespace. 

Example : to launch the device x 

```
~/multi-ue.sh -c x
~/multi-ue.sh -o x
```

```
cd openairinterface5g/cmake_targets/ran_build/build

sudo ./nr-uesoftmodem -r 106 --numerology 1 --band 78 -C 3619200000 --uicc0.imsi 00101000000000x --rfsim --device_id x --brokerip 10.20x.1.100

```




