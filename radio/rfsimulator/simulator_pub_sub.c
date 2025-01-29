/*
* Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
* contributor license agreements.  See the NOTICE file distributed with
* this work for additional information regarding copyright ownership.
* The OpenAirInterface Software Alliance licenses this file to You under
* the OAI Public License, Version 1.1  (the "License"); you may not use this file
* except in compliance with the License.
* You may obtain a copy of the License at
*
*      http://www.openairinterface.org/?page_id=698
*
* Author and copyright: Laurent Thomas, open-cells.com
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*-------------------------------------------------------------------------------
* For more information about the OpenAirInterface (OAI) Software Alliance:
*      contact@openairinterface.org
*/

/*
 * PUB SUB TODO: 
 * - zmq_poll() failed, errno(88)
 * - Tear down
 * - Socket loss detection,
 */
/*
 * Open issues and limitations
 * The read and write should be called in the same thread, that is not new USRP UHD design
 * When the opposite side switch from passive reading to active R+Write, the synchro is not fully deterministic
 */

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/epoll.h>
#include <netdb.h>
#include <zmq.h>
#include <common/utils/assertions.h>
#include <common/utils/LOG/log.h>
#include <common/utils/load_module_shlib.h>
#include <common/utils/telnetsrv/telnetsrv.h>
#include <common/config/config_userapi.h>
#include "common_lib.h"
#define CHANNELMOD_DYNAMICLOAD
#include <openair1/SIMULATION/TOOLS/sim.h>
#include "rfsimulator.h"
#include "hashtable.h"
#include <sys/time.h>
#define PORT 4043 //default TCP port for this simulator
#define XSUBPORT 5555
#define XPUBPORT 5556
//
// CirSize defines the number of samples inquired for a read cycle
// It is bounded by a slot read capability (which depends on bandwidth and numerology)
// up to multiple slots read to allow I/Q buffering of the I/Q TCP stream
//
// As a rule of thumb:
// -it can't be less than the number of samples for a slot
// -it can range up to multiple slots
//
// The default value is chosen for 10ms buffering which makes 23040*20 = 460800 samples
// The previous value is kept below in comment it was computed for 100ms 1x 20MHz
// #define CirSize 6144000 // 100ms SiSo 20MHz LTE
// #define minCirSize 460800 // 10ms  SiSo 40Mhz 3/4 sampling NR78 FR1
// #define minCirSize 86080000
#define minCirSize 50080000
// #define minCirSize 20080000

#define sampleToByte(a,b) ((a)*(b)*sizeof(sample_t))
#define byteToSample(a,b) ((a)/(sizeof(sample_t)*(b)))

#define GENERATE_CHANNEL 10 // each frame (or slot?) in DL

// This needs to be re-architected in the future
//
// File Descriptors management in rfsimulator is not optimized
// Relying on FD_SETSIZE (actually 1024) is not appropriated
// Also the use of fd value as returned by Linux as an index for buf[] structure is not appropriated
// especially for client (UE) side since only 1 fd per connection to a gNB is needed. On the server
// side the value should be tuned to the maximum number of connections with UE's which corresponds
// to the maximum number of UEs hosted by a gNB which is unlikely to be in the order of thousands
// since all I/Q's would flow through the same TCP transport.
// Until a convenient management is implemented, the MAX_FD_RFSIMU is used everywhere (instead of
// FD_SETSIE) and reduced to 125. This should allow for around 20 simultaeous UEs.
//
// An indirection level via hashtable was added to allow the software to use FDs above MAX_FD_RFSIMU.
//
// #define MAX_FD_RFSIMU FD_SETSIZE
#define MAX_FD_RFSIMU 250
#define SEND_BUFF_SIZE 100000000 // Socket buffer size

// Simulator role
typedef enum { SIMU_ROLE_SERVER = 1, SIMU_ROLE_CLIENT } simuRole; //SIMU_ROLE_CLIENT is 2 implicitly

//

#define RFSIMU_SECTION    "rfsimulator"
#define RFSIMU_OPTIONS_PARAMNAME "options"


#define RFSIM_CONFIG_HELP_OPTIONS     " list of comma separated options to enable rf simulator functionalities. Available options: \n"\
  "        chanmod:   enable channel modelisation\n"\
  "        saviq:     enable saving written iqs to a file\n"
/*-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------*/
/*                                            configuration parameters for the rfsimulator device                                                                              */
/*   optname                     helpstr                     paramflags           XXXptr                               defXXXval                          type         numelt  */
/*-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------*/
#define simOpt PARAMFLAG_NOFREE|PARAMFLAG_CMDLINE_NOPREFIXENABLED
#define RFSIMULATOR_PARAMS_DESC {					\
    {"brokerip",             "<broker ip address to connect to>\n",        simOpt,  .strptr=&rfsimulator->brokerip,               .defstrval="0.0.0.0",           TYPE_STRING,    0 },\
    {"device_id",             "<device id>\n",                                 simOpt,  .strptr=&rfsimulator->device_id,            .defstrval="0",           TYPE_STRING,     0 },\
    {"xsubport",             "<port to connect to xsubsocket>\n",              simOpt,  .u16ptr=&(rfsimulator->xsubport),           .defuintval=XSUBPORT,                 TYPE_UINT16,    0 },\
    {"xpubport",             "<port to connect to xpubsocket>\n",              simOpt,  .u16ptr=&(rfsimulator->xpubport),           .defuintval=XPUBPORT,                 TYPE_UINT16,    0 },\
    {"serveraddr",             "<ip address to connect to>\n",        simOpt,  .strptr=&rfsimulator->ip,               .defstrval="127.0.0.1",           TYPE_STRING,    0 },\
    {RFSIMU_OPTIONS_PARAMNAME, RFSIM_CONFIG_HELP_OPTIONS,             0,       .strlistptr=NULL,                       .defstrlistval=NULL,              TYPE_STRINGLIST,0 },\
    {"IQfile",                 "<file path to use when saving IQs>\n",simOpt,  .strptr=&saveF,                         .defstrval="/tmp/rfsimulator.iqs",TYPE_STRING,    0 },\
    {"modelname",              "<channel model name>\n",              simOpt,  .strptr=&modelname,                     .defstrval="AWGN",                TYPE_STRING,    0 },\
    {"ploss",                  "<channel path loss in dB>\n",         simOpt,  .dblptr=&(rfsimulator->chan_pathloss),  .defdblval=0,                     TYPE_DOUBLE,    0 },\
    {"forgetfact",             "<channel forget factor ((0 to 1)>\n", simOpt,  .dblptr=&(rfsimulator->chan_forgetfact),.defdblval=0,                     TYPE_DOUBLE,    0 },\
    {"offset",                 "<channel offset in samps>\n",         simOpt,  .u64ptr=&(rfsimulator->chan_offset),    .defint64val=0,                   TYPE_UINT64,    0 },\
    {"prop_delay",             "<propagation delay in ms>\n",         simOpt,  .dblptr=&(rfsimulator->prop_delay_ms),  .defdblval=0.0,                   TYPE_DOUBLE,    0 },\
    {"wait_timeout",           "<wait timeout if no UE connected>\n", simOpt,  .iptr=&(rfsimulator->wait_timeout),     .defintval=1,                     TYPE_INT,       0 },\
  };

static void getset_currentchannels_type(char *buf, int debug, webdatadef_t *tdata, telnet_printfunc_t prnt);
extern int get_currentchannels_type(char *buf, int debug, webdatadef_t *tdata, telnet_printfunc_t prnt); // in random_channel.c
static int rfsimu_setchanmod_cmd(char *buff, int debug, telnet_printfunc_t prnt, void *arg);
static int rfsimu_setdistance_cmd(char *buff, int debug, telnet_printfunc_t prnt, void *arg);
static int rfsimu_getdistance_cmd(char *buff, int debug, telnet_printfunc_t prnt, void *arg);
static int rfsimu_vtime_cmd(char *buff, int debug, telnet_printfunc_t prnt, void *arg);
static telnetshell_cmddef_t rfsimu_cmdarray[] = {
    {"show models", "", (cmdfunc_t)rfsimu_setchanmod_cmd, {(webfunc_t)getset_currentchannels_type}, TELNETSRV_CMDFLAG_WEBSRVONLY | TELNETSRV_CMDFLAG_GETWEBTBLDATA, NULL},
    {"setmodel", "<model name> <model type>", (cmdfunc_t)rfsimu_setchanmod_cmd, {NULL}, TELNETSRV_CMDFLAG_PUSHINTPOOLQ | TELNETSRV_CMDFLAG_TELNETONLY, NULL},
    {"setdistance", "<model name> <distance>", (cmdfunc_t)rfsimu_setdistance_cmd, {NULL}, TELNETSRV_CMDFLAG_PUSHINTPOOLQ | TELNETSRV_CMDFLAG_NEEDPARAM },
    {"getdistance", "<model name>", (cmdfunc_t)rfsimu_getdistance_cmd, {NULL}, TELNETSRV_CMDFLAG_PUSHINTPOOLQ},
    {"vtime", "", (cmdfunc_t)rfsimu_vtime_cmd, {NULL}, TELNETSRV_CMDFLAG_PUSHINTPOOLQ | TELNETSRV_CMDFLAG_AUTOUPDATE},
    {"", "", NULL},
};
static telnetshell_cmddef_t *setmodel_cmddef = &(rfsimu_cmdarray[1]);

static telnetshell_vardef_t rfsimu_vardef[] = {{"", 0, 0, NULL}};
pthread_mutex_t Sockmutex;
unsigned int nb_ue = 0;

static uint64_t CirSize = minCirSize;

bool receivedFirstTS = false;
typedef c16_t sample_t; // 2*16 bits complex number

typedef struct buffer_s {
  int fd_pub_sock; 
  int fd_sub_sock;
  int conn_device_id; // integer
  openair0_timestamp lastReceivedTS;
  bool headerMode;
  bool trashingPacket;
  samplesBlockHeader_t th;
  char *transferPtr;
  uint64_t remainToTransfer;
  char *circularBufEnd;
  sample_t *circularBuf;
  channel_desc_t *channel_model;
} buffer_t;

typedef struct {
  openair0_timestamp nextRxTstamp;
  openair0_timestamp lastWroteTS;
  simuRole role;
  char *device_id;

  void *context;
  void *pub_sock;
  void *sub_sock;
  int fd_pub_sock;
  int fd_sub_sock;

  char *brokerip;
  uint16_t xsubport; 
  uint16_t xpubport;
  char *ip;
  uint16_t port;
  int saveIQfile;
  buffer_t buf[MAX_FD_RFSIMU];
  int next_buf;
  // Hashtable used as an indirection level between device_id and the buf array
  hash_table_t *id_to_buf_map;
  int rx_num_channels;
  int tx_num_channels;
  double sample_rate;
  double rx_freq;
  double tx_bw;
  int channelmod;
  double chan_pathloss;
  double chan_forgetfact;
  uint64_t chan_offset;
  float  noise_power_dB;
  void *telnetcmd_qid;
  poll_telnetcmdq_func_t poll_telnetcmdq;
  int wait_timeout;
  double prop_delay_ms;
} rfsimulator_state_t;


static buffer_t *get_buff_from_id(rfsimulator_state_t *simulator_state, int id)
{
  uint64_t buffer_index;
  if (hashtable_get(simulator_state->id_to_buf_map, id, (void **)&buffer_index) == HASH_TABLE_OK) {
    return &simulator_state->buf[buffer_index];
  } else {
    return NULL;
  }
}


static void add_buff_to_id_mapping(rfsimulator_state_t *simulator_state, int id, uint64_t buff_index)
{
  hashtable_rc_t rc = hashtable_insert(simulator_state->id_to_buf_map, id, (void *)buff_index);
  AssertFatal(rc == HASH_TABLE_OK,
              "%s id = %d\n",
              rc == HASH_TABLE_INSERT_OVERWRITTEN_DATA ? "Duplicate entry in hashtable" : "Hashtable is not allocated",
              id);
}


static void remove_buff_to_id_mapping(rfsimulator_state_t *simulator_state, int id)
{
  // Failure is fine here
  hashtable_remove(simulator_state->id_to_buf_map, id);
}

static int allocCirBuf(rfsimulator_state_t *bridge, int id)
{
  /* TODO: cleanup code so that this AssertFatal becomes useless */
  AssertFatal(id >= 0 && id < sizeofArray(bridge->buf), "device_id %d is not in range\n", id);
  uint64_t buff_index = bridge->next_buf++ % MAX_FD_RFSIMU;
  buffer_t *ptr=&bridge->buf[buff_index];
  ptr->circularBuf = calloc(1, sampleToByte(CirSize, 1));
  if (ptr->circularBuf == NULL) {
    LOG_E(HW, "malloc(%lu) failed\n", sampleToByte(CirSize, 1));
    return -1;
  }
  ptr->circularBufEnd=((char *)ptr->circularBuf)+sampleToByte(CirSize,1);
  ptr->conn_device_id = id; 
  ptr->fd_pub_sock = bridge->fd_pub_sock;
  ptr->fd_sub_sock = bridge->fd_sub_sock;
  ptr->lastReceivedTS=0;
  ptr->headerMode=true;
  ptr->trashingPacket=false;
  ptr->transferPtr=(char *)&ptr->th;
  ptr->remainToTransfer=sizeof(samplesBlockHeader_t);
  int sendbuff = SEND_BUFF_SIZE;

  size_t optlen = sizeof(sendbuff);
  if (zmq_setsockopt(bridge->pub_sock, ZMQ_SNDBUF, &sendbuff, optlen)!=0){
    LOG_E(HW, "zmq_setsockopt(SO_SNDBUF) failed\n");
    return -1;
  }
  if (bridge->role == SIMU_ROLE_SERVER) { 
    int rcvhwm = 0;
    if (zmq_setsockopt(bridge->sub_sock, ZMQ_RCVHWM, &rcvhwm, sizeof(int))!=0){
      LOG_E(HW, "zmq_setsockopt(ZMQ_RCVHWM) failed\n");
      return -1;
    };
  }
  if ( bridge->channelmod > 0) {
    // create channel simulation model for this mode reception
    static bool init_done=false;

    if (!init_done) {
      uint64_t rand;
      FILE *h=fopen("/dev/random","r");

      if ( 1 != fread(&rand,sizeof(rand),1,h) )
        LOG_W(HW, "Can't read /dev/random\n");

      fclose(h);
      randominit(rand);
      tableNor(rand);
      init_done=true;
    }
    char *modelname = (bridge->role == SIMU_ROLE_SERVER) ? "rfsimu_channel_ue0" : "rfsimu_channel_enB0";
    ptr->channel_model = find_channel_desc_fromname(modelname); // path_loss in dB
    if (!ptr->channel_model) {
      LOG_E(HW, "Channel model %s not found, check config file\n", modelname);
      return -1;
    }

    set_channeldesc_owner(ptr->channel_model, RFSIMU_MODULEID);
    random_channel(ptr->channel_model,false);
    LOG_I(HW, "Random channel %s in rfsimulator activated\n", modelname);
  }
  add_buff_to_id_mapping(bridge, id, buff_index);
  return 0;
}

static void removeCirBuf(rfsimulator_state_t *bridge, int id) {
  buffer_t* buf = get_buff_from_id(bridge, id);
  if (buf) {
    free(buf->circularBuf);
    // Fixme: no free_channel_desc_scm(bridge->buf[sock].channel_model) implemented
    // a lot of mem leaks
    //free(bridge->buf[sock].channel_model);
    memset(buf, 0, sizeof(buffer_t));
    buf->fd_pub_sock=-1;
    buf->fd_sub_sock=-1;
    remove_buff_to_id_mapping(bridge, id);
    nb_ue--;
  }
}

// static void socketError(rfsimulator_state_t *bridge, int id) {
//   buffer_t* buf = get_buff_from_id(bridge, id);
//   if (!buf) return;
//   if (buf->fd_pub_sock != -1) {
//     LOG_W(HW, "Lost socket\n");
//     removeCirBuf(bridge, id);

//     if (bridge->role == SIMU_ROLE_CLIENT)
//       exit(1);
//   }
// }

static bool flushInput(rfsimulator_state_t *t, int timeout, int nsamps);

static int rfsimulator_write_internal(rfsimulator_state_t *t, openair0_timestamp timestamp, void **samplesVoid, int nsamps, int nbAnt, int flags, bool alreadyLocked);


static void fullwrite(void *pub_sock, void *_buf, ssize_t count, rfsimulator_state_t *t) {
  if (t->saveIQfile != -1) {
    if (write(t->saveIQfile, _buf, count) != count )
      LOG_E(HW, "write() in save iq file failed (%d)\n", errno);
  }

  char *buf = _buf;
  ssize_t l;
  // Sending topic
  while (count) {
    if (t->role == SIMU_ROLE_SERVER){
      char topic[] = "downlink";
      // zmq_send(pub_sock, topic, strlen(topic), ZMQ_SNDMORE | ZMQ_DONTWAIT);
      zmq_send(pub_sock, topic, strlen(topic), ZMQ_SNDMORE );
    }
    else {
      char topic[] = "uplink";
      char formatted_topic[256];

    // Format the topic with the device ID
      sprintf(formatted_topic, "%s %s", topic, t->device_id);
      // zmq_send(pub_sock, formatted_topic, strlen(formatted_topic), ZMQ_SNDMORE | ZMQ_DONTWAIT);
      zmq_send(pub_sock, formatted_topic, strlen(formatted_topic), ZMQ_SNDMORE);

    }

    // l = zmq_send(pub_sock, buf, count, ZMQ_DONTWAIT);
    l = zmq_send(pub_sock, buf, count, 0);

    if (l == 0) {
        LOG_E(HW, "write() failed, returned 0\n");
        return;
    }

    if (l < 0) {
      if (errno==EINTR)
        continue;

      if (errno == EAGAIN) {
        LOG_D(HW, "write() failed, errno(%d)\n", errno);
        usleep(250);
        continue;
      } else {
        LOG_E(HW, "write() failed, errno(%d)\n", errno);
        return;
      }
    }
    LOG_D(HW, "Successfully sent %zd bytes.\n", l);

    count -= l;
    buf += l;
  }
}

static void rfsimulator_readconfig(rfsimulator_state_t *rfsimulator) {
  char *saveF=NULL;
  char *modelname=NULL;
  paramdef_t rfsimu_params[] = RFSIMULATOR_PARAMS_DESC;
  int p = config_paramidx_fromname(rfsimu_params, sizeofArray(rfsimu_params), RFSIMU_OPTIONS_PARAMNAME);
  int ret = config_get(config_get_if(), rfsimu_params, sizeofArray(rfsimu_params), RFSIMU_SECTION);
  AssertFatal(ret >= 0, "configuration couldn't be performed\n");

  rfsimulator->saveIQfile = -1;

  for(int i=0; i<rfsimu_params[p].numelt ; i++) {
    if (strcmp(rfsimu_params[p].strlistptr[i],"saviq") == 0) {
      rfsimulator->saveIQfile=open(saveF,O_APPEND| O_CREAT|O_TRUNC | O_WRONLY, 0666);

      if ( rfsimulator->saveIQfile != -1 )
        LOG_D(HW, "Will save written IQ samples in %s\n", saveF);
      else {
        LOG_E(HW, "open(%s) failed for IQ saving, errno(%d)\n", saveF, errno);
        exit(-1);
      }

      break;
    } else if (strcmp(rfsimu_params[p].strlistptr[i],"chanmod") == 0) {
      init_channelmod();
      load_channellist(rfsimulator->tx_num_channels, rfsimulator->rx_num_channels, rfsimulator->sample_rate, rfsimulator->rx_freq, rfsimulator->tx_bw);
      rfsimulator->channelmod=true;
    } else {
      fprintf(stderr, "unknown rfsimulator option: %s\n", rfsimu_params[p].strlistptr[i]);
      exit(-1);
    }
  }

  /* for compatibility keep environment variable usage */
  if ( getenv("RFSIMULATOR") != NULL ) {
    rfsimulator->ip=getenv("RFSIMULATOR");
    LOG_W(HW, "The RFSIMULATOR environment variable is deprecated and support will be removed in the future. Instead, add parameter --rfsimulator.serveraddr %s to set the server address. Note: the default is \"server\"; for the gNB/eNB, you don't have to set any configuration.\n", rfsimulator->ip);
    LOG_D(HW, "Remove RFSIMULATOR environment variable to get rid of this message and the sleep.\n");
    sleep(10);
  }

  if ( strncasecmp(rfsimulator->ip,"enb",3) == 0 ||
       strncasecmp(rfsimulator->ip,"server",3) == 0 )
    rfsimulator->role = SIMU_ROLE_SERVER;
  else
    rfsimulator->role = SIMU_ROLE_CLIENT;
}

static int rfsimu_setchanmod_cmd(char *buff, int debug, telnet_printfunc_t prnt, void *arg) {
  char *modelname=NULL;
  char *modeltype=NULL;
  rfsimulator_state_t *t = (rfsimulator_state_t *)arg;
  if (t->channelmod == false) {
    prnt("%s: ERROR channel modelisation disabled...\n", __func__);
    return 0;
  }
  if (buff == NULL) {
    prnt("%s: ERROR wrong rfsimu setchannelmod command...\n", __func__);
    return 0;
  }
  if (debug)
    prnt("%s: rfsimu_setchanmod_cmd buffer \"%s\"\n", __func__, buff);
  int s = sscanf(buff,"%m[^ ] %ms\n",&modelname, &modeltype);

  if (s == 2) {
    int channelmod=modelid_fromstrtype(modeltype);

    if (channelmod<0)
      prnt("%s: ERROR: model type %s unknown\n", __func__, modeltype);
    else {
      rfsimulator_state_t *t = (rfsimulator_state_t *)arg;
      int found=0;
      for (int i = 0; i < MAX_FD_RFSIMU; i++) {
        buffer_t *b=&t->buf[i];
        if ( b->channel_model==NULL)
          continue;
        if (b->channel_model->model_name==NULL)
          continue;
        if (b->fd_pub_sock >= 0 && (strcmp(b->channel_model->model_name,modelname)==0)) {
          channel_desc_t *newmodel = new_channel_desc_scm(t->tx_num_channels,
                                                          t->rx_num_channels,
                                                          channelmod,
                                                          t->sample_rate,
                                                          t->rx_freq,
                                                          t->tx_bw,
                                                          30e-9, // TDL delay-spread parameter
                                                          0.0,
                                                          CORR_LEVEL_LOW,
                                                          t->chan_forgetfact, // forgetting_factor
                                                          t->chan_offset, // propagation delay in samples
                                                          t->chan_pathloss,
                                                          t->noise_power_dB); // path_loss in dB
          set_channeldesc_owner(newmodel, RFSIMU_MODULEID);
          set_channeldesc_name(newmodel,modelname);
          random_channel(newmodel,false);
          channel_desc_t *oldmodel=b->channel_model;
          b->channel_model=newmodel;
          free_channel_desc_scm(oldmodel);
          prnt("%s: New model type %s applied to channel %s connected to sock %d\n", __func__, modeltype, modelname, i);
          found=1;
          break;
        }
      } /* for */
      if (found==0)
        prnt("%s: Channel %s not found or not currently used\n", __func__, modelname);
    }
  } else {
    prnt("%s: ERROR: 2 parameters required: model name and model type (%i found)\n", __func__, s);
  }

  free(modelname);
  free(modeltype);
  return CMDSTATUS_FOUND;
}

static void getset_currentchannels_type(char *buf, int debug, webdatadef_t *tdata, telnet_printfunc_t prnt)
{
  if (strncmp(buf, "set", 3) == 0) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "setmodel %s %s", tdata->lines[0].val[1], tdata->lines[0].val[3]);
    push_telnetcmd_func_t push_telnetcmd = (push_telnetcmd_func_t)get_shlibmodule_fptr("telnetsrv", TELNET_PUSHCMD_FNAME);
    push_telnetcmd(setmodel_cmddef, cmd, prnt);
  } else {
    get_currentchannels_type("modify type", debug, tdata, prnt);
  }
}


static int rfsimu_setdistance_cmd(char *buff, int debug, telnet_printfunc_t prnt, void *arg)
{
  if (debug)
    prnt("%s() buffer \"%s\"\n", __func__, buff);

  char *modelname;
  int distance;
  int s = sscanf(buff,"%m[^ ] %d\n", &modelname, &distance);
  if (s != 2) {
    prnt("%s: require exact two parameters\n", __func__);
    return CMDSTATUS_VARNOTFOUND;
  }

  rfsimulator_state_t *t = (rfsimulator_state_t *)arg;
  const double sample_rate = t->sample_rate;
  const double c = 299792458; /* 3e8 */

  const uint64_t new_offset = (double) distance * sample_rate / c;
  const double new_distance = (double) new_offset * c / sample_rate;
  const double new_delay_ms = new_offset * 1000.0 / sample_rate;

  prnt("\n%s: new_offset %lu, new (exact) distance %.3f m, new delay %f ms\n", __func__, new_offset, new_distance, new_delay_ms);
  t->prop_delay_ms = new_delay_ms;
  t->chan_offset = new_offset;

  /* Set distance in rfsim and channel model, update channel and ringbuffer */
  for (int i = 0; i < MAX_FD_RFSIMU; i++) {
    buffer_t *b=&t->buf[i];
    if (b->fd_pub_sock <= 0 || b->channel_model == NULL || b->channel_model->model_name == NULL || strcmp(b->channel_model->model_name, modelname) != 0) {
      if (b->channel_model != NULL && b->channel_model->model_name != NULL)
        prnt("  %s: model %s unmodified\n", __func__, b->channel_model->model_name);
      continue;
    }

    channel_desc_t *cd = b->channel_model;
    cd->channel_offset = new_offset;
  }

  free(modelname);

  return CMDSTATUS_FOUND;
}

static int rfsimu_getdistance_cmd(char *buff, int debug, telnet_printfunc_t prnt, void *arg)
{
  if (debug)
    prnt("%s() buffer \"%s\"\n", __func__, (buff != NULL) ? buff : "NULL");

  rfsimulator_state_t *t = (rfsimulator_state_t *)arg;
  const double sample_rate = t->sample_rate;
  const double c = 299792458; /* 3e8 */

  for (int i = 0; i < MAX_FD_RFSIMU; i++) {
    buffer_t *b=&t->buf[i];
    if (b->fd_pub_sock <= 0 || b->channel_model == NULL || b->channel_model->model_name == NULL)
      continue;

    channel_desc_t *cd = b->channel_model;
    const uint64_t offset = cd->channel_offset;
    const double distance = (double) offset * c / sample_rate;
    prnt("%s: %s offset %lu distance %.3f m\n", __func__, cd->model_name, offset, distance);
  }
  prnt("%s: <default> offset %lu delay %f ms\n", __func__, t->chan_offset, t->prop_delay_ms);

  return CMDSTATUS_FOUND;
}

static int rfsimu_vtime_cmd(char *buff, int debug, telnet_printfunc_t prnt, void *arg)
{
  rfsimulator_state_t *t = (rfsimulator_state_t *)arg;
  const openair0_timestamp ts = t->nextRxTstamp;
  const double sample_rate = t->sample_rate;
  prnt("%s: vtime measurement: TS %llu sample_rate %.3f\n", __func__, ts, sample_rate);
  return CMDSTATUS_FOUND;
}

static int startServer(openair0_device *device)
{

  rfsimulator_state_t *t = (rfsimulator_state_t *)device->priv;
  t->role = SIMU_ROLE_SERVER;
  t->context = zmq_ctx_new();
  AssertFatal(t->context != NULL, "Failed to create ZeroMQ context");

  // Create the publisher socket
  t->pub_sock = zmq_socket(t->context, ZMQ_PUB);
  AssertFatal(t->pub_sock != NULL, "Failed to create publisher socket");

  // Set up monitoring for publisher socket to detect connection to the broker
  int rc = zmq_socket_monitor(t->pub_sock, "inproc://monitor.pub", ZMQ_EVENT_ALL);
  AssertFatal(rc == 0, "Failed to set up socket monitoring for publisher");

  void *pub_monitor = zmq_socket(t->context, ZMQ_PAIR);
  AssertFatal(pub_monitor != NULL, "Failed to create publisher monitor socket");
  rc = zmq_connect(pub_monitor, "inproc://monitor.pub");
  AssertFatal(rc == 0, "Failed to connect publisher monitor socket");

  // Create the subscriber socket
  t->sub_sock = zmq_socket(t->context, ZMQ_SUB);
  AssertFatal(t->sub_sock != NULL, "Failed to create subscriber socket");

  // Set up monitoring for subscriber socket to detect connection to the broker
  rc = zmq_socket_monitor(t->sub_sock, "inproc://monitor.sub", ZMQ_EVENT_ALL);
  AssertFatal(rc == 0, "Failed to set up socket monitoring for subscriber");

  void *sub_monitor = zmq_socket(t->context, ZMQ_PAIR);
  AssertFatal(sub_monitor != NULL, "Failed to create subscriber monitor socket");
  rc = zmq_connect(sub_monitor, "inproc://monitor.sub");
  AssertFatal(rc == 0, "Failed to connect subscriber monitor socket");

  // Connect the sockets to the broker
  char pub_endpoint[256];
  snprintf(pub_endpoint, sizeof(pub_endpoint), "tcp://%s:%d", t->brokerip, t->xsubport);
  rc = zmq_connect(t->pub_sock, pub_endpoint);
  AssertFatal(rc == 0, "Failed to connect publisher socket");

  char sub_endpoint[256];
  snprintf(sub_endpoint, sizeof(sub_endpoint), "tcp://%s:%d", t->brokerip, t->xpubport);
  rc = zmq_connect(t->sub_sock, sub_endpoint);
  AssertFatal(rc == 0, "Failed to connect subscriber socket");

  const char *topic = "uplink";
  rc = zmq_setsockopt(t->sub_sock, ZMQ_SUBSCRIBE, topic, strlen(topic));
  AssertFatal(rc == 0, "Failed to subscribe to topic");

  const char *topic2 = "join";
  rc = zmq_setsockopt(t->sub_sock, ZMQ_SUBSCRIBE, topic2, strlen(topic2));
  AssertFatal(rc == 0, "Failed to subscribe to topic");

  size_t fd_size = sizeof(t->fd_pub_sock);
  rc = zmq_getsockopt(t->pub_sock, ZMQ_FD, &t->fd_pub_sock, &fd_size);
  AssertFatal(rc == 0, "Cannot get fd for pub_sock");
  rc = zmq_getsockopt(t->sub_sock, ZMQ_FD, &t->fd_sub_sock, &fd_size);
  AssertFatal(rc == 0, "Cannot get fd for sub_sock");

  // Monitor the sockets to detect when they are connected
  bool pub_connected = false;
  bool sub_connected = false;
  struct timeval start, now;
  gettimeofday(&start, NULL);

  while (!pub_connected || !sub_connected) {
    // Process events for publisher
    printf("trying to connect to the broker\n");
    zmq_msg_t event_msg;
    zmq_msg_init(&event_msg);
    rc = zmq_msg_recv(&event_msg, pub_monitor, ZMQ_DONTWAIT);
    if (rc != -1) {
      uint16_t event = *(uint16_t *)zmq_msg_data(&event_msg);
      zmq_msg_close(&event_msg);

      if (event == ZMQ_EVENT_CONNECTED) {
        LOG_D(HW, "Publisher socket connected \n");
        pub_connected = true;
      }
    } else {
      zmq_msg_close(&event_msg);
    }

    // Process events for subscriber
    zmq_msg_init(&event_msg);
    rc = zmq_msg_recv(&event_msg, sub_monitor, ZMQ_DONTWAIT);
    if (rc != -1) {
      uint16_t event = *(uint16_t *)zmq_msg_data(&event_msg);
      zmq_msg_close(&event_msg);

      if (event == ZMQ_EVENT_CONNECTED) {
        LOG_D(HW, "Subscriber socket connected \n");
        sub_connected = true;
      }
    } else {
      zmq_msg_close(&event_msg);
    }
    // Avoid infinite loop
    gettimeofday(&now, NULL);
    double elapsed = (now.tv_sec - start.tv_sec) +
                     (now.tv_usec - start.tv_usec) / 1000000.0;
    if (elapsed > 7.0) {
        LOG_W(HW, "Waited more than 7 seconds for connection to the broker, exiting loop\n");
        break;
    }
    usleep(10000); 
  }

  LOG_I(HW, "Connection to the broker established\n");
  zmq_close(pub_monitor);
  zmq_close(sub_monitor);

  return 0;

}


static int startClient(openair0_device *device)
{

  rfsimulator_state_t *t = device->priv;
  t->role = SIMU_ROLE_CLIENT;
  t->context = zmq_ctx_new();
  AssertFatal(t->context != NULL, "Failed to create ZeroMQ context");

  // Create the publisher socket
  t->pub_sock = zmq_socket(t->context, ZMQ_PUB);
  AssertFatal(t->pub_sock != NULL, "Failed to create publisher socket");

  // Set up monitoring for publisher socket to detect connection to the broker
  int rc = zmq_socket_monitor(t->pub_sock, "inproc://monitor.pub", ZMQ_EVENT_ALL);
  AssertFatal(rc == 0, "Failed to set up socket monitoring for publisher");

  void *pub_monitor = zmq_socket(t->context, ZMQ_PAIR);
  AssertFatal(pub_monitor != NULL, "Failed to create publisher monitor socket");
  rc = zmq_connect(pub_monitor, "inproc://monitor.pub");
  AssertFatal(rc == 0, "Failed to connect publisher monitor socket");

  // Create the subscriber socket
  t->sub_sock = zmq_socket(t->context, ZMQ_SUB);
  AssertFatal(t->sub_sock != NULL, "Failed to create subscriber socket");

  // Set up monitoring for subscriber socket to detect connection to the broker
  rc = zmq_socket_monitor(t->sub_sock, "inproc://monitor.sub", ZMQ_EVENT_ALL);
  AssertFatal(rc == 0, "Failed to set up socket monitoring for subscriber");

  void *sub_monitor = zmq_socket(t->context, ZMQ_PAIR);
  AssertFatal(sub_monitor != NULL, "Failed to create subscriber monitor socket");
  rc = zmq_connect(sub_monitor, "inproc://monitor.sub");
  AssertFatal(rc == 0, "Failed to connect subscriber monitor socket");

  // Connect the sockets to the broker
  char pub_endpoint[256];
  snprintf(pub_endpoint, sizeof(pub_endpoint), "tcp://%s:%d", t->brokerip, t->xsubport);
  rc = zmq_connect(t->pub_sock, pub_endpoint);
  AssertFatal(rc == 0, "Failed to connect publisher socket");

  printf("connecting to endpoint: %s\n",pub_endpoint);

  char sub_endpoint[256];
  snprintf(sub_endpoint, sizeof(sub_endpoint), "tcp://%s:%d", t->brokerip, t->xpubport);
  rc = zmq_connect(t->sub_sock, sub_endpoint);
  AssertFatal(rc == 0, "Failed to connect subscriber socket");

  // Subscribe to the downlink topic
  const char *topic = "downlink";

  rc = zmq_setsockopt(t->sub_sock, ZMQ_SUBSCRIBE, topic, strlen(topic));
  AssertFatal(rc == 0, "Failed to subscribe to topic");
  size_t fd_size = sizeof(t->fd_pub_sock);
  rc = zmq_getsockopt(t->pub_sock, ZMQ_FD, &t->fd_pub_sock, &fd_size);
  AssertFatal(rc == 0, "Cannot get fd for pub_sock");
  rc = zmq_getsockopt(t->sub_sock, ZMQ_FD, &t->fd_sub_sock, &fd_size);
  AssertFatal(rc == 0, "Cannot get fd for sub_sock");

  // Monitor the sockets to detect when they are connected
  bool pub_connected = false;
  bool sub_connected = false;

  while (!pub_connected || !sub_connected) {
    // Process events for publisher
    printf("trying to connect to the broker\n");
    zmq_msg_t event_msg;
    zmq_msg_init(&event_msg);
    rc = zmq_msg_recv(&event_msg, pub_monitor, ZMQ_DONTWAIT);
    if (rc != -1) {
      uint16_t event = *(uint16_t *)zmq_msg_data(&event_msg);
      zmq_msg_close(&event_msg);

      if (event == ZMQ_EVENT_CONNECTED) {
        LOG_D(HW, "Publisher socket connected\n");
        pub_connected = true;
      }
    } else {
      zmq_msg_close(&event_msg);
    }

    // Process events for subscriber
    zmq_msg_init(&event_msg);
    rc = zmq_msg_recv(&event_msg, sub_monitor, ZMQ_DONTWAIT);
    if (rc != -1) {
      uint16_t event = *(uint16_t *)zmq_msg_data(&event_msg);
      zmq_msg_close(&event_msg);

      if (event == ZMQ_EVENT_CONNECTED) {
        LOG_D(HW, "Subscriber socket connected\n");
        sub_connected = true;
      }
    } else {
      zmq_msg_close(&event_msg);
    }

    usleep(10000); 
  }

  LOG_I(HW, "Connection to the broker established\n");
  zmq_close(pub_monitor);
  zmq_close(sub_monitor);
  char jointopic[] = "join";
  zmq_send(t->pub_sock, jointopic, strlen(jointopic), ZMQ_SNDMORE);
  zmq_send(t->pub_sock, t->device_id, strlen(t->device_id), 0);
  return allocCirBuf(t, atoi(t->device_id));

}

static int rfsimulator_write_internal(rfsimulator_state_t *t, openair0_timestamp timestamp, void **samplesVoid, int nsamps, int nbAnt, int flags, bool alreadyLocked) {
  if (!alreadyLocked)
    pthread_mutex_lock(&Sockmutex);

  LOG_D(HW, "Sending %d samples at time: %ld, nbAnt %d\n", nsamps, timestamp, nbAnt);
  // all connected UEs need to have a buffer to broadcast the data
  int count = 0;
  for (int i = 0; i < MAX_FD_RFSIMU; i++) {
    buffer_t *b=&t->buf[i];
    if (b->fd_pub_sock >=0) count++;
  }
  if ( ((count != 0) && (count == nb_ue)) || t->role ==SIMU_ROLE_CLIENT){
    if (t->fd_pub_sock >= 0) {
      samplesBlockHeader_t header = {nsamps, nbAnt, timestamp};
      fullwrite(t->pub_sock,&header, sizeof(header), t);
      sample_t tmpSamples[nsamps][nbAnt];

      if (nbAnt == 1) {
        if (t->fd_pub_sock >= 0) {
          fullwrite(t->pub_sock, samplesVoid[0], sampleToByte(nsamps, nbAnt), t);
        }
      } else {
        for (int a = 0; a < nbAnt; a++) {
          sample_t *in = (sample_t *)samplesVoid[a];

          for (int s = 0; s < nsamps; s++)
            tmpSamples[s][a] = in[s];
        }

        if (t->fd_pub_sock >= 0) {
          fullwrite(t->pub_sock, (void *)tmpSamples, sampleToByte(nsamps, nbAnt), t);
        }
      }
    }
  }

  if ( t->lastWroteTS != 0 && fabs((double)t->lastWroteTS-timestamp) > (double)CirSize)
    LOG_W(HW, "Discontinuous TX gap too large Tx:%lu, %lu\n", t->lastWroteTS, timestamp);

  if (t->lastWroteTS > timestamp)
    LOG_W(HW, "Not supported to send Tx out of order %lu, %lu\n", t->lastWroteTS, timestamp);

  if ((flags != TX_BURST_START) && (flags != TX_BURST_START_AND_END) && (t->lastWroteTS < timestamp))
    LOG_W(HW,
          "Gap in writing to USRP: last written %lu, now %lu, gap %lu\n",
          t->lastWroteTS,
          timestamp,
          timestamp - t->lastWroteTS);

  t->lastWroteTS=timestamp+nsamps;

  if (!alreadyLocked)
    pthread_mutex_unlock(&Sockmutex);

  LOG_D(HW,
        "Sent %d samples at time: %ld->%ld, energy in first antenna: %d\n",
        nsamps,
        timestamp,
        timestamp + nsamps,
        signal_energy(samplesVoid[0], nsamps));
  return nsamps;
}

static int rfsimulator_write(openair0_device *device, openair0_timestamp timestamp, void **samplesVoid, int nsamps, int nbAnt, int flags) {
  timestamp -= device->openair0_cfg->command_line_sample_advance;
  return rfsimulator_write_internal(device->priv, timestamp, samplesVoid, nsamps, nbAnt, flags, false); // false = with lock
  // return rfsimulator_write_internal(device->priv, timestamp, samplesVoid, nsamps, nbAnt, flags, true);
}

static bool flushInput(rfsimulator_state_t *t, int timeout, int nsamps_for_initial) {
  // Process all incoming events on socket
  // store the data in lists
  zmq_pollitem_t items[] = {
        { t->sub_sock, 0, ZMQ_POLLIN, 0 }// maybe this should be moved to another function
    };
  int rc = zmq_poll(items, 1, timeout);
  if (rc < 0) {
    if (errno == EINTR || errno == ETERM) {
    return false;
  }
  LOG_W(HW, "zmq_poll() failed, errno(%d)\n", errno);
  return false;
}

  if (rc == 0) {
    return false;
  }

  if (items[0].revents & ZMQ_POLLIN) {
      //receiving topic
      char topic[256];
      int cap = sizeof(topic);
      // int tsize= zmq_recv(t->sub_sock, topic,cap-1 , ZMQ_DONTWAIT);
      int tsize= zmq_recv(t->sub_sock, topic,cap-1 , 0);
      if ( tsize < 0 ) {
        if ( errno != EAGAIN ) {
          LOG_E(HW, "zmq_recv() failed, errno(%d)\n", errno);
          AssertFatal(false,"Failed in reading the topic\n");
          //abort();
        }
      }
      topic[tsize < cap ? tsize : cap - 1] = '\0';
      LOG_D(HW,"received topic %s\n",topic);
      if (strncasecmp(topic, "join", 3) == 0){
        if ( t->role == SIMU_ROLE_SERVER ) {
          char deviceid[256];
          int cap = sizeof(deviceid);
          // int idsize= zmq_recv(t->sub_sock, deviceid,cap-1 , ZMQ_DONTWAIT);
          int idsize= zmq_recv(t->sub_sock, deviceid,cap-1 , 0);
          if ( idsize < 0 ) {
            if ( errno != EAGAIN ) {
              LOG_E(HW, "zmq_recv() failed, errno(%d)\n", errno);
              AssertFatal(false,"Failed in reading the device id\n");
              }
          }
          deviceid[idsize < cap ? idsize : cap - 1] = '\0';
          LOG_D(HW,"received device_id %s\n",deviceid);
          int device_id = atoi(deviceid);
          if (allocCirBuf(t, device_id) == -1) {
              return false;
            }
          LOG_I(HW, "A client connects: %s, sending the current time\n",deviceid);
          c16_t v= {0};
          nb_ue++;
          void *samplesVoid[t->tx_num_channels];

          for ( int i=0; i < t->tx_num_channels; i++)
            samplesVoid[i]=(void *)&v;

          rfsimulator_write_internal(t, t->lastWroteTS > 1 ? t->lastWroteTS - 1 : 0, samplesVoid, 1, t->tx_num_channels, 1, false);

          buffer_t *b = get_buff_from_id(t, device_id);
          if (b->channel_model)
            b->channel_model->start_TS = t->lastWroteTS;
          return rc > 0;
        }
      }
      buffer_t *b = NULL;
      if (t->role == SIMU_ROLE_SERVER) { // receiving formatted topic = topic + device_id 
        char deviceid[256];
        sscanf(topic, "uplink %255s", deviceid);
        int id = atoi(deviceid);
        b = get_buff_from_id(t, id);
      } else {
        b = get_buff_from_id(t, atoi(t->device_id));
      }

      if (!b) return rc > 0;
      if ( b->circularBuf == NULL ) {
        LOG_E(HW, "Received data on not connected socket \n");
        return rc > 0;
      }
      ssize_t blockSz;

      if ( b->headerMode )
        blockSz=b->remainToTransfer; 
      else
        blockSz= b->transferPtr + b->remainToTransfer <= b->circularBufEnd ?
                 b->remainToTransfer :
                 b->circularBufEnd - b->transferPtr ;

      //receiving data ( iq samples ) or header
      ssize_t sz = zmq_recv(t->sub_sock, b->transferPtr, blockSz, ZMQ_DONTWAIT);
      if (sz == 24 && !receivedFirstTS) {
        receivedFirstTS = true;
        LOG_D(HW,"recieved firstTS: %d\n",receivedFirstTS);
      // receiving of previously queued up messages in the receive buffer
      }else if (!receivedFirstTS) {
        LOG_W(HW,"Ignoring old queued up messages\n");
        return rc > 0;
      }
      // ssize_t sz = zmq_recv(t->sub_sock, b->transferPtr, blockSz, 0);
      LOG_D(HW, "Received on topic %s , nbr %zd bytes\n", topic, sz);
      if ( sz < 0 ) {
        if ( errno != EAGAIN ) {
          LOG_E(HW, "zmq_recv() failed, errno(%d)\n", errno);
          //abort();
        }
      }
      if (sz > 0 ) {
          LOG_D(HW, "Received on topic %s, %zd bytes\n", topic, sz);
          b->remainToTransfer -= sz;
          b->transferPtr+=sz;

          if (b->transferPtr==b->circularBufEnd )
            b->transferPtr=(char *)b->circularBuf;

          // check the header and start block transfer
          if ( b->headerMode==true && b->remainToTransfer==0) {
            b->headerMode= false;

            if (t->nextRxTstamp == 0 ) { // First block in UE, resync with the gNB current TS
              t->nextRxTstamp=b->th.timestamp> nsamps_for_initial ?
                              b->th.timestamp -  nsamps_for_initial :
                              0;
              b->lastReceivedTS=b->th.timestamp> nsamps_for_initial ?
                                b->th.timestamp :
                                nsamps_for_initial;
              LOG_I(HW, "UE got first timestamp: starting at %lu\n", t->nextRxTstamp);
              b->trashingPacket=true;
              if (b->channel_model)
                b->channel_model->start_TS = t->nextRxTstamp;
            } else if (b->lastReceivedTS < b->th.timestamp) {
              int nbAnt= b->th.nbAnt;

              if ( b->th.timestamp-b->lastReceivedTS < CirSize ) {
                for (uint64_t index=b->lastReceivedTS; index < b->th.timestamp; index++ ) {
                  for (int a=0; a < nbAnt; a++) {
                    b->circularBuf[(index*nbAnt+a)%CirSize].r = 0;
                    b->circularBuf[(index*nbAnt+a)%CirSize].i = 0;
                  }
                }
              } else {
                memset(b->circularBuf, 0, sampleToByte(CirSize,1));
              }

              b->lastReceivedTS=b->th.timestamp;
            } else if (b->lastReceivedTS > b->th.timestamp && b->th.size == 1) {
              LOG_W(HW, "Received Rx/Tx synchro out of order\n");
              b->trashingPacket=true;
            } else if (b->lastReceivedTS == b->th.timestamp) {
              // normal case
            } else {
              LOG_W(HW, "Received data in past: current is %lu, new reception: %lu!\n", b->lastReceivedTS, b->th.timestamp);
              b->trashingPacket=true;
            }

            pthread_mutex_lock(&Sockmutex);

            if (t->lastWroteTS != 0 && (fabs((double)t->lastWroteTS-b->lastReceivedTS) > (double)CirSize))
              LOG_W(HW, "UEsock(sub_sock) Tx/Rx shift too large Tx:%lu, Rx:%lu\n", t->lastWroteTS, b->lastReceivedTS);

            pthread_mutex_unlock(&Sockmutex);
            b->transferPtr=(char *)&b->circularBuf[(b->lastReceivedTS*b->th.nbAnt)%CirSize];
            b->remainToTransfer=sampleToByte(b->th.size, b->th.nbAnt);
          }

          if ( b->headerMode==false ) {
            if ( ! b->trashingPacket ) {
              b->lastReceivedTS=b->th.timestamp+b->th.size-byteToSample(b->remainToTransfer,b->th.nbAnt);
              LOG_D(HW, "UEsock: sub_sock Set b->lastReceivedTS %ld\n", b->lastReceivedTS);
            }

            if ( b->remainToTransfer==0) {
              LOG_D(HW, "UEsock: sub_sock Completed block reception: %ld\n", b->lastReceivedTS);
              b->headerMode=true;
              b->transferPtr=(char *)&b->th;
              b->remainToTransfer = sizeof(samplesBlockHeader_t);
              b->trashingPacket=false;
            }
          }
        }
    }
  return rc > 0;
}

static int rfsimulator_read(openair0_device *device, openair0_timestamp *ptimestamp, void **samplesVoid, int nsamps, int nbAnt)
{
  rfsimulator_state_t *t = device->priv;
  LOG_D(HW, "Enter rfsimulator_read, expect %d samples, will release at TS: %ld, nbAnt %d\n", nsamps, t->nextRxTstamp+nsamps, nbAnt);

  // deliver data from received data
  // check if a UE is connected

  int first_sock;
  for (first_sock = 0; first_sock < MAX_FD_RFSIMU; first_sock++)
    if (t->buf[first_sock].circularBuf != NULL )
      break;

  if (first_sock == MAX_FD_RFSIMU) {
    if ( t->nextRxTstamp == 0)
        LOG_I(HW, "No connected device, generating void samples...\n");

      if (!flushInput(t, t->wait_timeout,  nsamps)) {
        for (int x=0; x < nbAnt; x++)
          memset(samplesVoid[x],0,sampleToByte(nsamps,1));

        t->nextRxTstamp+=nsamps;

        if ( ((t->nextRxTstamp/nsamps)%100) == 0)
          LOG_D(HW, "No UE, Generating void samples for Rx: %ld\n", t->nextRxTstamp);

        *ptimestamp = t->nextRxTstamp-nsamps;
        return nsamps;
      }
  } else {
    bool have_to_wait;

    do {
      have_to_wait=false;

      buffer_t *b = NULL;
      for (int id = 0; id < MAX_FD_RFSIMU; id++) {
        b = &t->buf[id];

        if ( b->circularBuf )
          if ( t->nextRxTstamp+nsamps > b->lastReceivedTS ) {
            have_to_wait=true;
            break;
          }
      }

      if (have_to_wait) {
        LOG_D(HW,
              "Waiting on socket, current last ts: %ld, expected at least : %ld\n",
              b->lastReceivedTS,
              t->nextRxTstamp + nsamps);
        flushInput(t, 3, nsamps);
      }
    } while (have_to_wait);
  }

  // Clear the output buffer
  for (int a=0; a<nbAnt; a++)
    memset(samplesVoid[a],0,sampleToByte(nsamps,1));

  // Add all input nodes signal in the output buffer
  for (int id = 0; id < MAX_FD_RFSIMU; id++) {
    buffer_t *ptr=&t->buf[id];

    if ( ptr->circularBuf ) {
      bool reGenerateChannel=false;

      //fixme: when do we regenerate
      // it seems legacy behavior is: never in UL, each frame in DL
      if (reGenerateChannel)
        random_channel(ptr->channel_model,0);

      if (t->poll_telnetcmdq)
        t->poll_telnetcmdq(t->telnetcmd_qid,t);

      for (int a=0; a<nbAnt; a++) {//loop over number of Rx antennas
        if ( ptr->channel_model != NULL ) { // apply a channel model
          rxAddInput(ptr->circularBuf, (c16_t *) samplesVoid[a],
                     a,
                     ptr->channel_model,
                     nsamps,
                     t->nextRxTstamp,
                     CirSize);
        }
        else { // no channel modeling
          int nbAnt_tx = ptr->th.nbAnt; // number of Tx antennas
          int firstIndex = (CirSize + t->nextRxTstamp - t->chan_offset) % CirSize;
          sample_t *out = (sample_t *)samplesVoid[a];
          if ((nbAnt_tx == 1) && ((nb_ue == 1) || (t->role == SIMU_ROLE_CLIENT))) { // optimized for 1 Tx and 1 UE
            sample_t *firstSample = (sample_t *)&(ptr->circularBuf[firstIndex]);
            if (firstIndex + nsamps > CirSize) {
              int tailSz = CirSize - firstIndex;
              memcpy(out, firstSample, sampleToByte(tailSz, 1));
              memcpy(out + tailSz, &ptr->circularBuf[0], sampleToByte(nsamps - tailSz, 1));
            } else {
              memcpy(out, firstSample, sampleToByte(nsamps, 1));
            }
          } else {
            // SIMD (with simde) optimization might be added here later
            double H_awgn_mimo[4][4] = {{1.0, 0.2, 0.1, 0.05}, // rx 0
                                        {0.2, 1.0, 0.2, 0.1}, // rx 1
                                        {0.1, 0.2, 1.0, 0.2}, // rx 2
                                        {0.05, 0.1, 0.2, 1.0}}; // rx 3

            LOG_D(HW, "nbAnt_tx %d\n", nbAnt_tx);
            for (int i = 0; i < nsamps; i++) { // loop over nsamps
              for (int a_tx = 0; a_tx < nbAnt_tx; a_tx++) { // sum up signals from nbAnt_tx antennas
                out[i].r += (short)(ptr->circularBuf[((firstIndex + i) * nbAnt_tx + a_tx) % CirSize].r * H_awgn_mimo[a][a_tx]);
                out[i].i += (short)(ptr->circularBuf[((firstIndex + i) * nbAnt_tx + a_tx) % CirSize].i * H_awgn_mimo[a][a_tx]);
              } // end for a_tx
            } // end for i (number of samps)
          } // end of 1 tx antenna optimization
        } // end of no channel modeling
      } // end for a (number of rx antennas)
    }
  }

  *ptimestamp = t->nextRxTstamp; // return the time of the first sample
  t->nextRxTstamp+=nsamps;
  LOG_D(HW,
        "Rx to upper layer: %d from %ld to %ld, energy in first antenna %d\n",
        nsamps,
        *ptimestamp,
        t->nextRxTstamp,
        signal_energy(samplesVoid[0], nsamps));
  return nsamps;
}

static int rfsimulator_get_stats(openair0_device *device) {
  return 0;
}
static int rfsimulator_reset_stats(openair0_device *device) {
  return 0;
}
static void rfsimulator_end(openair0_device *device) {
  rfsimulator_state_t* s = device->priv;
  zmq_close(s->sub_sock);
  zmq_close(s->pub_sock);
  zmq_ctx_destroy(s->context);
  for (int i = 0; i < MAX_FD_RFSIMU; i++) {
    buffer_t *b = &s->buf[i];
    if (b->fd_pub_sock >= 0 )
      removeCirBuf(s, b->conn_device_id);
  }
  s->fd_pub_sock=-1;
  s->fd_sub_sock=-1;
  hashtable_destroy(&s->id_to_buf_map);
}
static int rfsimulator_stop(openair0_device *device) {
  return 0;
}
static int rfsimulator_set_freq(openair0_device *device, openair0_config_t *openair0_cfg) {
  rfsimulator_state_t* s = device->priv;
  s->rx_freq = openair0_cfg->rx_freq[0];
  return 0;
}
static int rfsimulator_set_gains(openair0_device *device, openair0_config_t *openair0_cfg) {
  return 0;
}
static int rfsimulator_write_init(openair0_device *device) {
  return 0;
}

void do_not_free_integer(void *integer)
{
  (void)integer;
}

__attribute__((__visibility__("default")))
int device_init(openair0_device *device, openair0_config_t *openair0_cfg) {
  // to change the log level, use this on command line
  // --log_config.hw_log_level debug
  rfsimulator_state_t *rfsimulator = calloc(sizeof(rfsimulator_state_t), 1);
  // initialize channel simulation
  rfsimulator->tx_num_channels=openair0_cfg->tx_num_channels;
  rfsimulator->rx_num_channels=openair0_cfg->rx_num_channels;
  rfsimulator->sample_rate=openair0_cfg->sample_rate;
  rfsimulator->rx_freq=openair0_cfg->rx_freq[0];
  rfsimulator->tx_bw=openair0_cfg->tx_bw;  
  rfsimulator_readconfig(rfsimulator);
  if (rfsimulator->prop_delay_ms > 0.0)
    rfsimulator->chan_offset = ceil(rfsimulator->sample_rate * rfsimulator->prop_delay_ms / 1000);
  if (rfsimulator->chan_offset != 0) {
    if (CirSize < minCirSize + rfsimulator->chan_offset) {
      CirSize = minCirSize + rfsimulator->chan_offset;
      LOG_I(HW, "CirSize = %lu\n", CirSize);
    }
    rfsimulator->prop_delay_ms = rfsimulator->chan_offset * 1000 / rfsimulator->sample_rate;
    LOG_D(HW, "propagation delay %f ms, %lu samples\n", rfsimulator->prop_delay_ms, rfsimulator->chan_offset);
  }
  pthread_mutex_init(&Sockmutex, NULL);
  LOG_D(HW,
        "Running as %s\n",
        rfsimulator->role == SIMU_ROLE_SERVER ? "server waiting opposite rfsimulators to connect"
                                              : "client: will connect to a rfsimulator server side");
  device->trx_start_func = rfsimulator->role == SIMU_ROLE_SERVER ? startServer : startClient;
  device->trx_get_stats_func   = rfsimulator_get_stats;
  device->trx_reset_stats_func = rfsimulator_reset_stats;
  device->trx_end_func         = rfsimulator_end;
  device->trx_stop_func        = rfsimulator_stop;
  device->trx_set_freq_func    = rfsimulator_set_freq;
  device->trx_set_gains_func   = rfsimulator_set_gains;
  device->trx_write_func       = rfsimulator_write;
  device->trx_read_func      = rfsimulator_read;
  /* let's pretend to be a b2x0 */
  device->type = RFSIMULATOR;
  openair0_cfg[0].rx_gain[0] = 0;
  device->openair0_cfg=&openair0_cfg[0];
  device->priv = rfsimulator;
  device->trx_write_init = rfsimulator_write_init;

  for (int i = 0; i < MAX_FD_RFSIMU; i++) {
    rfsimulator->buf[i].fd_sub_sock = -1;
    rfsimulator->buf[i].fd_pub_sock = -1;
  }

  rfsimulator->fd_pub_sock = -1;
  rfsimulator->fd_sub_sock = -1;  
  rfsimulator->next_buf = 0;
  
  rfsimulator->id_to_buf_map = hashtable_create(MAX_FD_RFSIMU, NULL, do_not_free_integer);

  // we need to call randominit() for telnet server (use gaussdouble=>uniformrand)
  randominit(0);
  set_taus_seed(0);
  /* look for telnet server, if it is loaded, add the channel modeling commands to it */
  add_telnetcmd_func_t addcmd = (add_telnetcmd_func_t)get_shlibmodule_fptr("telnetsrv", TELNET_ADDCMD_FNAME);

  if (addcmd != NULL) {
    rfsimulator->poll_telnetcmdq =  (poll_telnetcmdq_func_t)get_shlibmodule_fptr("telnetsrv", TELNET_POLLCMDQ_FNAME);
    addcmd("rfsimu",rfsimu_vardef,rfsimu_cmdarray);

    for(int i=0; rfsimu_cmdarray[i].cmdfunc != NULL; i++) {
      if (  rfsimu_cmdarray[i].qptr != NULL) {
        rfsimulator->telnetcmd_qid = rfsimu_cmdarray[i].qptr;
        break;
      }
    }
  }

  /* write on a socket fails if the other end is closed and we get SIGPIPE */
  if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
    perror("SIGPIPE");
    exit(1);
  }

  return 0;
}