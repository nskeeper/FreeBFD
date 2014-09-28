#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <libconfig.h>
#include <inttypes.h>
#include "bfd.h"
#include "bfd-monitor.h"
#include "bfdLog.h"
#include "bfdExtensions.h"
#include "tp-timers.h"

/*
 * Command line usage info
 */
static void bfddUsage(void)
{
  int idx;

  fprintf(stderr, "Usage:\n");
  fprintf(stderr, "\tbfdd [options] -c config-file [-v]\n");
  fprintf(stderr, "Where:\n");
  fprintf(stderr, "\t-c: load 'config-file' for startup configuration\n");
  fprintf(stderr, "Options:\n");
  fprintf(stderr, "\t-d: Do not run in daemon mode\n");
  fprintf(stderr, "\t-m port: Port monitor server will listen on (default %d)\n",
          DEFAULT_MONITOR_PORT);
  fprintf(stderr, "\t-v: increase level of debug output (can be repeated)\n");
  fprintf(stderr, "\t-x extension: enable a named extension (can be repeated)\n");
  for (idx=0; idx < BFD_EXT_MAX; idx++) {
    const char* name;
    const char* desc;

    bfdExtDescribe(idx, &name, &desc);
    fprintf(stderr, "\t\t%s\t%s\n", name, desc);
  }
  fprintf(stderr, "\n");
  fprintf(stderr, "Signals:\n");
  fprintf(stderr, "\tUSR1: start poll sequence on all demand mode sessions\n");
  fprintf(stderr, "\tUSR2: toggle admin down on all sessions\n");
}

/*
 * Main entry point of process
 */
int main(int argc, char **argv)
{
  int c;
  char *configFile = NULL;
  int daemon_mode = 1;
  uint16_t monitor_port = DEFAULT_MONITOR_PORT;

  config_t cfg;
  config_setting_t *sns;
  config_setting_t *exts;

  bfdLogInit();

  /* Get command line options */
  while ((c = getopt(argc, argv, "c:dm:vx:")) != -1) {
    switch (c) {
    case 'c':
      configFile = optarg;
      break;
    case 'd':
      daemon_mode = 0;
      break;
    case 'm':
      if (sscanf(optarg, "%" SCNu16, &monitor_port) != 1) {
        fprintf(stderr, "Expected integer for monitor port.\n");
        bfddUsage();
        exit(1);
      }
      break;
    case 'v':
      bfdLogMore();
      break;
    case 'x':
      if (!bfdExtEnable(optarg)) {
        fprintf(stderr, "Invalid extension: %s\n", optarg);
        bfddUsage();
        exit(1);
      }
      break;
    default:
      bfddUsage();
      exit(1);
    }
  }

  /* Must have specified peer address */
  if (configFile == NULL) {
    bfddUsage();
    exit(1);
  }

  if (daemon_mode) {
    if (daemon(1, 0) != 0) {
      bfdLog(LOG_ERR, "Unable to daemonize!");
      exit(1);
    }
  }

  /* Init random() */
  srandom((unsigned int)time(NULL));

  /* Init timers package */
  tpInitTimers();

  /* Set signal handlers */
  tpSetSignalActor(bfdStartPollSequence, SIGUSR1);
  tpSetSignalActor(bfdToggleAdminDown, SIGUSR2);

  config_init(&cfg);

  /* Read the file */
  if(!config_read_file(&cfg, configFile)) {            
    bfdLog(LOG_ERR, "Error loading config file [%s]: %s:%d - %s\n",
           configFile,
           config_error_file(&cfg),
           config_error_line(&cfg),
           config_error_text(&cfg));
    config_destroy(&cfg);
    exit(1);
  }

  /* Parse extensions */
  if ((exts = config_lookup(&cfg, "Extensions")) != NULL) {
    int32_t cnt = config_setting_length(exts);
    uint32_t i;

    for (i=0; i<cnt; i++) {
      const char* extName;
      bool extVal;

      config_setting_t *ext = config_setting_get_elem(exts, i);

      if ((extName = config_setting_name(ext)) == NULL) {
        bfdLog(LOG_WARNING, "Unnamed extension [%d] - ignoring\n", i);
        continue;
      }

      extVal = config_setting_get_bool(ext);

      if (!extVal) { continue; }

      if (!bfdExtEnable(extName)) {
        bfdLog(LOG_WARNING,
               "Attempt to enable unknown extension [%s] - ignoring\n",
               extName);
        continue;
      }
    }
  }

  /* Parse configured sessions */
  if ((sns = config_lookup(&cfg, "Sessions")) != NULL) {
    int32_t cnt = config_setting_length(sns);
    uint32_t i;

    for (i=0; i<cnt; i++) {
      struct hostent *hp;
      struct in_addr peeraddr;
      const char *connectaddr = NULL;
      int32_t peerPort;
      int32_t localport;
      int32_t demandMode;
      int32_t detectMult;
      int32_t reqMinRx;
      int32_t desMinTx;
      bfdSession *bfd;

      config_setting_t *sn = config_setting_get_elem(sns, i);

      if (!config_setting_lookup_string(sn, "PeerAddress", &connectaddr)) {
        bfdLog(LOG_WARNING,
               "Session %d missing PeerAddress - Skipping Session!\n", i);
        continue;
      }

      if (config_setting_lookup_int(sn, "PeerPort", &peerPort)) {
        if ((uint32_t)peerPort & 0xffff0000) {
          bfdLog(LOG_WARNING,
                 "Session %d PeerPort out of range: %d - Skipping Session!\n",
                 i, peerPort);
          continue;
        }

        if (!bfdExtCheck(BFD_EXT_SPECIFYPORTS)) {
          bfdLog(LOG_WARNING,
                 "Invalid remote port: %d - Skipping Session!\n", peerPort);
          bfdLog(LOG_WARNING,
                 "Did you forget to enable the SpecifyPorts extension?\n");
          continue;
        }
      } else {
        peerPort = 3784;
      }

      if (config_setting_lookup_int(sn, "LocalPort", &localport)) {
        if ((uint32_t)localport & 0xffff0000) {
          bfdLog(LOG_WARNING, "Session %d LocalPort out of range: %d - Skipping Session!\n",
                 i, localport);
          continue;
        }

        if (!bfdExtCheck(BFD_EXT_SPECIFYPORTS)) {
          bfdLog(LOG_WARNING,
                 "Invalid local port: %d - Skipping Session!\n", localport);
          bfdLog(LOG_WARNING,
                 "Did you forget to enable the SpecifyPorts extension?\n");
          continue;
        }
      } else {
        localport = 3784;
      }

      if (!config_setting_lookup_bool(sn, "DemandMode", &demandMode)) {
        demandMode = 0;
      }

      if (config_setting_lookup_int(sn, "DetectMult", &detectMult)) {
        if ((uint32_t)detectMult & 0xffffff00) {
          bfdLog(LOG_ERR, "Session %d DetectMult out of range: %d - Skipping Session!\n",
                 i, localport);
          continue;
        }
      } else {
        detectMult = BFDDFLT_DETECTMULT;
      }

      if (!config_setting_lookup_int(sn, "RequiredMinRxInterval", &reqMinRx)) {
        reqMinRx = BFDDFLT_REQUIREDMINRX;
      }

      if (!config_setting_lookup_int(sn, "DesiredMinTxInterval", &desMinTx)) {
        desMinTx = BFDDFLT_DESIREDMINTX;
      }

      bfdLog(LOG_NOTICE,
             "BFD[%d]: demandModeDesired %s, detectMult %d, desiredMinTx %d, requiredMinRx %d\n",
             i, (demandMode ? "on" : "off"), detectMult, desMinTx, reqMinRx);

      /* Get peer address */
      if ((hp = gethostbyname(connectaddr)) == NULL) {
        bfdLog(LOG_ERR, "Can't resolve %s: %s\n", connectaddr, hstrerror(h_errno));
        exit(1);
      }

      if (hp->h_addrtype != AF_INET) {
        bfdLog(LOG_ERR, "Resolved address type not AF_INET\n");
        exit(1);
      }

      memcpy(&peeraddr, hp->h_addr, sizeof(peeraddr));

      /* Make the initial session */
      bfdLog(LOG_INFO, "Creating initial session with %s (%s)\n", connectaddr,
             inet_ntoa(peeraddr));

      /* Get memory */
      if ((bfd = (bfdSession*)malloc(sizeof(bfdSession))) == NULL) {
        bfdLog(LOG_NOTICE, "Can't malloc memory for new session: %m\n");
        exit(1);
      }

      memset(bfd, 0, sizeof(bfdSession));

      bfd->DemandMode            = (uint8_t)(demandMode & 0x1);
      bfd->DetectMult            = (uint8_t)detectMult;
      bfd->DesiredMinTxInterval  = (uint32_t)desMinTx;
      bfd->RequiredMinRxInterval = (uint32_t)reqMinRx;
      bfd->PeerAddr              = peeraddr;
      bfd->PeerPort              = (uint16_t)peerPort;
      bfd->LocalPort             = (uint16_t)localport;

      if (!bfdRegisterSession(bfd)) {
        bfdLog(LOG_ERR, "Can't create initial session: %m\n");
        exit(1);
      }
    }
  }

  config_destroy(&cfg);

  bfdMonitorSetupServer(monitor_port);

  /* Wait for events */
  tpDoEventLoop();

  /* Should never return */
  exit(1);
}