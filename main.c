/******************************************************************************
  @file    main.c
  @brief   The entry program.

  DESCRIPTION
  Connectivity Management Tool for USB network adapter of Quectel wireless cellular modules.

  INITIALIZATION AND SEQUENCING REQUIREMENTS
  None.

  ---------------------------------------------------------------------------
  Copyright (c) 2016 -2020 Quectel Wireless Solution, Co., Ltd.  All Rights Reserved.
  Quectel Wireless Solution Proprietary and Confidential.
  ---------------------------------------------------------------------------
******************************************************************************/

#include "QMIThread.h"
#include <sys/wait.h>
#include <sys/utsname.h>
#include <sys/time.h>
#include <dirent.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include "util.h"
//#define CONFIG_PID_FILE_FORMAT "/var/run/quectel-CM-%s.pid" //for example /var/run/quectel-CM-wwan0.pid

char *apnConfigfile = NULL;
int debug_qmi = 0;
int qmidevice_control_fd[2];
static int signal_control_fd[2];

extern int ql_ifconfig(int argc, char *argv[]);
extern int ql_get_netcard_driver_info(const char*);
extern int ql_capture_usbmon_log(PROFILE_T *profile, const char *log_path);
extern void ql_stop_usbmon_log(PROFILE_T *profile);


// get apn config from apn-conf.xml file
void parseApn(xmlNode *node, const char *searchMcc, const char *searchMnc, PROFILE_T *profile) {
    char *mcc = NULL;
    char *mnc = NULL;
    char *apn = NULL;
    char *user = NULL;
    char *password = NULL;

    xmlAttr *attribute = node->properties;
    while (attribute) {
        if (strcmp((const char *)attribute->name, "mcc") == 0) {
            mcc = (char *)attribute->children->content;
        } else if (strcmp((const char *)attribute->name, "mnc") == 0) {
            mnc = (char *)attribute->children->content;
        } else if (strcmp((const char *)attribute->name, "apn") == 0) {
            apn = (char *)attribute->children->content;
        } else if (strcmp((const char *)attribute->name, "user") == 0) {
            user = (char *)attribute->children->content;
        } else if (strcmp((const char *)attribute->name, "password") == 0) {
            password = (char *)attribute->children->content;
        }
        attribute = attribute->next;
    }

    if (mcc && mnc && strcmp(mcc, searchMcc) == 0 && strcmp(mnc, searchMnc) == 0) {
        if (strstr(apn, "mms") == NULL) {
          if (profile) {
            if (apn) {
                profile->apn = strdup(apn);
            }
            if (user) profile->user = strdup(user);
            if (password) profile->password = strdup(password);
          }
            //printf("APN: %s\n", apn ? apn : "N/A");
            //printf("User: %s\n", user ? user : "N/A");
            //printf("Password: %s\n", password ? password : "N/A");
        }
    }
}

void parseXml(const char *filename, const char *searchMcc, const char *searchMnc, PROFILE_T *profile) {
    xmlDoc *doc = xmlReadFile(filename, NULL, 0);
    if (doc == NULL) {
        printf("Could not parse the XML file\n");
        return;
    }

    xmlNode *rootElement = xmlDocGetRootElement(doc);
    xmlNode *currentNode = NULL;

    for (currentNode = rootElement->children; currentNode; currentNode = currentNode->next) {
        if (currentNode->type == XML_ELEMENT_NODE && strcmp((const char *)currentNode->name, "apn") == 0) {
            parseApn(currentNode, searchMcc, searchMnc, profile);
        }
    }

    xmlFreeDoc(doc);
    xmlCleanupParser();
}

//UINT ifc_get_addr(const char *ifname);
static int s_link = -1;
static void usbnet_link_state(int state)
{
    s_link = state ? 1 : 0;
}
static void usbnet_link_change(int link, PROFILE_T *profile) {
    if (s_link == link)
        return;

    s_link = link;

    if (!(link & (1<<IpFamilyV4)))
        memset(&profile->ipv4, 0, sizeof(IPV4_T));

    if (!(link & (1<<IpFamilyV6)))
        memset(&profile->ipv6, 0, sizeof(IPV6_T));

    if (link) {
        udhcpc_start(profile);
    } else {
        udhcpc_stop(profile);
    }
}

static int check_ipv4_address(PROFILE_T *profile) {
    uint32_t oldAddress = profile->ipv4.Address;

    if (profile->request_ops == &mbim_request_ops)
        return 1; //we will get a new ipv6 address per requestGetIPAddress()
    if (profile->request_ops == &atc_request_ops)
        return 1; //TODO

    if (profile->request_ops->requestGetIPAddress(profile, IpFamilyV4) == 0) {
         if (profile->ipv4.Address != oldAddress || debug_qmi) {
             unsigned char *l = (unsigned char *)&oldAddress;
             unsigned char *r = (unsigned char *)&profile->ipv4.Address;
             dbg_time("localIP: %d.%d.%d.%d VS remoteIP: %d.%d.%d.%d",
                     l[3], l[2], l[1], l[0], r[3], r[2], r[1], r[0]);
        }
        return (profile->ipv4.Address == oldAddress);
    }

    return 0;
}

static void main_send_event_to_qmidevice(int triger_event) {
     if (write(qmidevice_control_fd[0], &triger_event, sizeof(triger_event)) == -1) {};
}

static void send_signo_to_main(int signo) {
     if (write(signal_control_fd[0], &signo, sizeof(signo)) == -1) {};
}

void qmidevice_send_event_to_main(int triger_event) {
     if (write(qmidevice_control_fd[1], &triger_event, sizeof(triger_event)) == -1) {};
}

void qmidevice_send_event_to_main_ext(int triger_event, void *data, unsigned len) {
     if (write(qmidevice_control_fd[1], &triger_event, sizeof(triger_event)) == -1) {};
     if (write(qmidevice_control_fd[1], data, len) == -1) {};
}

#define MAX_PATH 256

static int ls_dir(const char *dir, int (*match)(const char *dir, const char *file, void *argv[]), void *argv[])
{
    DIR *pDir;
    struct dirent* ent = NULL;
    int match_times = 0;

    pDir = opendir(dir);
    if (pDir == NULL)  {
        dbg_time("Cannot open directory: %s, errno: %d (%s)", dir, errno, strerror(errno));
        return 0;
    }

    while ((ent = readdir(pDir)) != NULL)  {
        match_times += match(dir, ent->d_name, argv);
    }
    closedir(pDir);

    return match_times;
}

static int is_same_linkfile(const char *dir, const char *file,  void *argv[])
{
    const char *qmichannel = (const char *)argv[1];
    char linkname[MAX_PATH];
    char filename[MAX_PATH];
    int linksize;

    snprintf(linkname, MAX_PATH, "%s/%s", dir, file);
    linksize = readlink(linkname, filename, MAX_PATH);
    if (linksize <= 0)
        return 0;

    filename[linksize] = 0;
    if (strcmp(filename, qmichannel))
        return 0;

    dbg_time("%s -> %s", linkname, filename);
    return 1;
}

static int is_brother_process(const char *dir, const char *file, void *argv[])
{
    //const char *myself = (const char *)argv[0];
    char linkname[MAX_PATH];
    char filename[MAX_PATH];
    int linksize;
    int i = 0, kill_timeout = 15;
    pid_t pid;

    //dbg_time("%s", file);
    while (file[i]) {
        if (!isdigit(file[i]))
            break;
        i++;
    }

    if (file[i]) {
        //dbg_time("%s not digit", file);
        return 0;
    }

    snprintf(linkname, MAX_PATH, "%s/%s/exe", dir, file);
    linksize = readlink(linkname, filename, MAX_PATH);
    if (linksize <= 0)
        return 0;

    filename[linksize] = 0;

    pid = atoi(file);
    if (pid >= getpid())
        return 0;

    snprintf(linkname, MAX_PATH, "%s/%s/fd", dir, file);
    if (!ls_dir(linkname, is_same_linkfile, argv))
        return 0;

    dbg_time("%s/%s/exe -> %s", dir, file, filename);
    while (kill_timeout-- && !kill(pid, 0))
    {
        kill(pid, SIGTERM);
        sleep(1);
    }
    if (!kill(pid, 0))
    {
        dbg_time("force kill %s/%s/exe -> %s", dir, file, filename);
        kill(pid, SIGKILL);
        sleep(1);
    }

    return 1;
}

static int kill_brothers(const char *qmichannel)
{
    char myself[MAX_PATH];
    int filenamesize;
    void *argv[2] = {myself, (void *)qmichannel};

    filenamesize = readlink("/proc/self/exe", myself, MAX_PATH);
    if (filenamesize <= 0)
        return 0;
    myself[filenamesize] = 0;

    if (ls_dir("/proc", is_brother_process, argv))
        sleep(1);

    return 0;
}

static int kill_data_call_pdp(int pdp, char *self) {
    int pid;
    char *p = NULL;

    p = self;
    while (*self) {
        if (*self == '/')
            p = self+1;
        self++;
    }

    pid = getpid_by_pdp(pdp, p);
    if (pid > 0) {
        dbg_time("send SIGINT to process %d", pid);
        return kill(pid, SIGINT);
    }

    return -1;
}

static void ql_sigaction(int signo) {
     if (SIGALRM == signo)
         send_signo_to_main(SIG_EVENT_START);
     else
     {
        send_signo_to_main(SIG_EVENT_STOP);
        main_send_event_to_qmidevice(SIG_EVENT_STOP); //main may be wating qmi response
    }
}

static pthread_t gQmiThreadID = 0;

static int usage(const char *progname) {
    dbg_time("Usage: %s [options]", progname);
    dbg_time("-s [apn [user password auth]]          Set apn/user/password/auth get from your network provider. auth: 1~pap, 2~chap");
    dbg_time("-c apn-config-file                     Set apns-conf.xml");
    dbg_time("-p pincode                             Verify sim card pin if sim card is locked");
    dbg_time("-p [quectel-][qmi|mbim]-proxy          Request to use proxy");
    dbg_time("-f logfilename                         Save log message of this program to file");
    dbg_time("-u usbmonlog filename                  Save usbmon log to file");
    dbg_time("-i interface                           Specify which network interface to setup data call when multi-modems exits");
    dbg_time("-4                                     Setup IPv4 data call (default)");
    dbg_time("-6                                     Setup IPv6 data call");
    dbg_time("-n pdn                                 Specify which pdn to setup data call (default 1 for QMI, 0 for MBIM)");
    dbg_time("-k pdn                                 Specify which pdn to hangup data call (by send SIGINT to 'quectel-CM -n pdn')");
    dbg_time("-m iface-idx                           Bind QMI data call to wwan0_<iface idx> when QMAP used. E.g '-n 7 -m 1' bind pdn-7 data call to wwan0_1");
    dbg_time("-b                                     Enable network interface bridge function (default 0)");
    dbg_time("-v                                     Verbose log mode, for debug purpose.");
    dbg_time("[Examples]");
    dbg_time("Example 1: %s ", progname);
    dbg_time("Example 2: %s -s 3gnet ", progname);
    dbg_time("Example 3: %s -s 3gnet carl 1234 1 -p 1234 -f gobinet_log.txt", progname);
    return 0;
}

int qmi_main(PROFILE_T *profile)
{
    int triger_event = 0;
    int signo;
#ifdef CONFIG_SIM
    SIM_Status SIMStatus = SIM_NOT_READY;
#endif
    UCHAR PSAttachedState = 0;
    UCHAR  IPv4ConnectionStatus = QWDS_PKT_DATA_UNKNOW;
    UCHAR  IPv6ConnectionStatus = QWDS_PKT_DATA_UNKNOW; 
    unsigned SetupCallFail = 0;
    unsigned long SetupCallAllowTime = clock_msec();
    int qmierr = 0;
    const struct request_ops *request_ops = profile ->request_ops;
    
    char *pp_imsi = NULL;
    char mcc_tmp[4] = {'\0'};
    char mnc_tmp[3] = {'\0'};

    /* signal trigger quit event */
    signal(SIGINT, ql_sigaction);
    signal(SIGTERM, ql_sigaction);
    /* timer routine */
    signal(SIGALRM, ql_sigaction);

//sudo apt-get install udhcpc
//sudo apt-get remove ModemManager
__main_loop:
    if (profile->reattach_flag) {
        if (!reattach_driver(profile)) 
            sleep(2);
    }

    /* try to recreate FDs*/
    if (socketpair( AF_LOCAL, SOCK_STREAM, 0, signal_control_fd) < 0 ) {
        dbg_time("%s Faild to create main_control_fd: %d (%s)", __func__, errno, strerror(errno));
        return -1;
    }

    if ( socketpair( AF_LOCAL, SOCK_STREAM, 0, qmidevice_control_fd ) < 0 ) {
        dbg_time("%s Failed to create thread control socket pair: %d (%s)", __func__, errno, strerror(errno));
        return 0;
    }

    if ((profile->qmap_mode == 0 || profile->qmap_mode == 1) && (!profile->proxy[0])) {
        kill_brothers(profile->qmichannel);
     }

    if (pthread_create( &gQmiThreadID, 0, profile->qmi_ops->read, (void *)profile) != 0) {
        dbg_time("%s Failed to create QMIThread: %d (%s)", __func__, errno, strerror(errno));
            return 0;
    }

    if ((read(qmidevice_control_fd[0], &triger_event, sizeof(triger_event)) != sizeof(triger_event))
        || (triger_event != RIL_INDICATE_DEVICE_CONNECTED)) {
        dbg_time("%s Failed to init QMIThread: %d (%s)", __func__, errno, strerror(errno));
        return 0;
    }

    if (profile->qmi_ops->init && profile->qmi_ops->init(profile)) {
        dbg_time("%s Failed to qmi init: %d (%s)", __func__, errno, strerror(errno));
        return 0;
    }

    if (request_ops->requestGetIMEI)
        request_ops->requestGetIMEI();

    if (request_ops->requestBaseBandVersion)
        request_ops->requestBaseBandVersion(profile);

    if (request_ops->requestSetEthMode)
        request_ops->requestSetEthMode(profile);

    if (request_ops->requestSetLoopBackState && profile->loopback_state) {
    	request_ops->requestSetLoopBackState(profile->loopback_state, profile->replication_factor);
    	profile->loopback_state = 0;
    }

    if (request_ops->requestGetSIMStatus) {
        qmierr = request_ops->requestGetSIMStatus(&SIMStatus);

        while (qmierr == QMI_ERR_OP_DEVICE_UNSUPPORTED /*|| (SIMStatus != SIM_READY)*/) {
            sleep(1);
            qmierr = request_ops->requestGetSIMStatus(&SIMStatus);
            dbg_time("[%s] SIMStatus %d", __func__, SIMStatus);
        }

        if ((SIMStatus == SIM_PIN) && profile->pincode && request_ops->requestEnterSimPin) {
            request_ops->requestEnterSimPin(profile->pincode);
        }
    }

    if (SIMStatus == SIM_READY) {
        if (request_ops->requestGetICCID)
            request_ops->requestGetICCID();

        if (request_ops->requestGetIMSI)
            request_ops->requestGetIMSI(&pp_imsi);
        dbg_time("[%s] IMSI %s", __func__, pp_imsi);
        if (pp_imsi && !(profile->apn || profile->user || profile->password) && apnConfigfile) {
          strncpy(mcc_tmp, pp_imsi, 3);
          strncpy(mnc_tmp, pp_imsi + 3, 2);
          parseXml(apnConfigfile, mcc_tmp, mnc_tmp, profile);
          dbg_time("[%s] APN is %s", __func__, profile->apn);
        }
        if (pp_imsi) {
          free(pp_imsi);
        }
    }

    if (request_ops->requestSetProfile && (profile->apn || profile->user || profile->password)) {
        request_ops->requestSetProfile(profile);
    }

    if (request_ops->requestGetProfile)
        request_ops->requestGetProfile(profile);

    request_ops->requestRegistrationState(&PSAttachedState);

    send_signo_to_main(SIG_EVENT_CHECK);

    while (1)
    {
        struct pollfd pollfds[] = {{signal_control_fd[1], POLLIN, 0}, {qmidevice_control_fd[0], POLLIN, 0}};
        int ne, ret, nevents = sizeof(pollfds)/sizeof(pollfds[0]);

        do {
            ret = poll(pollfds, nevents,  15*1000);
        } while ((ret < 0) && (errno == EINTR));

        if (ret == 0)
        {
            send_signo_to_main(SIG_EVENT_CHECK);
            continue;
        }

        if (ret <= 0) {
            dbg_time("%s poll=%d, errno: %d (%s)", __func__, ret, errno, strerror(errno));
            goto __main_quit;
        }

        for (ne = 0; ne < nevents; ne++) {
            int fd = pollfds[ne].fd;
            short revents = pollfds[ne].revents;

            if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
                dbg_time("%s poll err/hup", __func__);
                dbg_time("epoll fd = %d, events = 0x%04x", fd, revents);
                main_send_event_to_qmidevice(RIL_REQUEST_QUIT);
                if (revents & POLLHUP)
                    goto __main_quit;
            }

            if ((revents & POLLIN) == 0)
                continue;

            if (fd == signal_control_fd[1])
            {
                if (read(fd, &signo, sizeof(signo)) == sizeof(signo))
                {
                    alarm(0);
                    switch (signo)
                    {
                        case SIG_EVENT_START:
                            if (PSAttachedState != 1 && profile->loopback_state == 0)
                                break;
                            
                            if (SetupCallAllowTime > clock_msec()) {
                                alarm((SetupCallAllowTime - clock_msec()+999)/1000);
                                break;
                            }

                            if (profile->enable_ipv4 && IPv4ConnectionStatus !=  QWDS_PKT_DATA_CONNECTED) {
                                qmierr = request_ops->requestSetupDataCall(profile, IpFamilyV4);

                                if ((qmierr > 0) && profile->user && profile->user[0] && profile->password && profile->password[0]) {
                                    int old_auto =  profile->auth;

                                    //may be fail because wrong auth mode, try pap->chap, or chap->pap
                                    profile->auth = (profile->auth == 1) ? 2 : 1;
                                	qmierr = request_ops->requestSetupDataCall(profile, IpFamilyV4);

                                    if (qmierr)
                                        profile->auth = old_auto; //still fail, restore old auth moe
                                }

                                if (!qmierr) {
                                    qmierr = request_ops->requestGetIPAddress(profile, IpFamilyV4);
                                    if (!qmierr)
                                        IPv4ConnectionStatus = QWDS_PKT_DATA_CONNECTED;
                                }
                                        
                            }

                            if (profile->enable_ipv6 && IPv6ConnectionStatus !=  QWDS_PKT_DATA_CONNECTED) {
                                if (profile->enable_ipv4 && profile->request_ops != &qmi_request_ops) {
                                    IPv6ConnectionStatus = IPv4ConnectionStatus;
                                }
                                else {
                                    qmierr = request_ops->requestSetupDataCall(profile, IpFamilyV6);

                                    if (!qmierr) {
                                        qmierr = request_ops->requestGetIPAddress(profile, IpFamilyV6);
                                        if (!qmierr)
                                            IPv6ConnectionStatus = QWDS_PKT_DATA_CONNECTED;
                                    }
                                }
                            }
                                
                            if ((profile->enable_ipv4 && IPv4ConnectionStatus ==  QWDS_PKT_DATA_DISCONNECTED)
                                    || (profile->enable_ipv6 && IPv6ConnectionStatus ==  QWDS_PKT_DATA_DISCONNECTED)) {
                                const unsigned allow_time[] = {5, 10, 20, 40, 60};

                                if (SetupCallFail < (sizeof(allow_time)/sizeof(unsigned)))
                                    SetupCallAllowTime = allow_time[SetupCallFail];
                                else
                                    SetupCallAllowTime = 60;
                                SetupCallFail++;
                                dbg_time("try to requestSetupDataCall %ld second later", SetupCallAllowTime);
                                alarm(SetupCallAllowTime);
                                SetupCallAllowTime = SetupCallAllowTime*1000 + clock_msec();
                            }
                            else if (IPv4ConnectionStatus ==  QWDS_PKT_DATA_CONNECTED || IPv6ConnectionStatus ==  QWDS_PKT_DATA_CONNECTED) {
                                SetupCallFail = 0;
                                SetupCallAllowTime = clock_msec();
                            }
                        break;

                        case SIG_EVENT_CHECK:
                            if (request_ops->requestGetSignalInfo)
                                request_ops->requestGetSignalInfo();

                            if (profile->enable_ipv4 && IPv4ConnectionStatus != QWDS_PKT_DATA_DISCONNECTED
                                && !request_ops->requestQueryDataCall(&IPv4ConnectionStatus, IpFamilyV4))
                            {
                                if (QWDS_PKT_DATA_CONNECTED == IPv4ConnectionStatus && profile->ipv4.Address == 0) {
                                    //killall -9 quectel-CM for MBIM and ATC call
                                    qmierr = request_ops->requestGetIPAddress(profile, IpFamilyV4);
                                    if (qmierr)
                                        IPv4ConnectionStatus = QWDS_PKT_DATA_DISCONNECTED;
                                }

                                //local ip is different with remote ip
                                if (QWDS_PKT_DATA_CONNECTED == IPv4ConnectionStatus && check_ipv4_address(profile) == 0) {
                                    request_ops->requestDeactivateDefaultPDP(profile, IpFamilyV4);
                                    IPv4ConnectionStatus = QWDS_PKT_DATA_DISCONNECTED;
                                }
                            }
                            else {
                                IPv4ConnectionStatus = QWDS_PKT_DATA_DISCONNECTED;
                            }

                            if (profile->enable_ipv6 && IPv6ConnectionStatus != QWDS_PKT_DATA_DISCONNECTED) {
                                if (profile->enable_ipv4 && profile->request_ops != &qmi_request_ops) {
                                    IPv6ConnectionStatus = IPv4ConnectionStatus;
                                }
                                else {
                                    request_ops->requestQueryDataCall(&IPv6ConnectionStatus, IpFamilyV6);
                                }
                            }
                            else {
                                IPv6ConnectionStatus = QWDS_PKT_DATA_DISCONNECTED;
                            }

                            if (IPv4ConnectionStatus ==  QWDS_PKT_DATA_DISCONNECTED && IPv6ConnectionStatus ==  QWDS_PKT_DATA_DISCONNECTED) {
                                usbnet_link_change(0, profile);
                            }
                            else if (IPv4ConnectionStatus ==  QWDS_PKT_DATA_CONNECTED || IPv6ConnectionStatus ==  QWDS_PKT_DATA_CONNECTED) {
                                int link = 0;
                                if (IPv4ConnectionStatus == QWDS_PKT_DATA_CONNECTED)
                                    link |= (1<<IpFamilyV4);
                                if (IPv6ConnectionStatus == QWDS_PKT_DATA_CONNECTED)
                                    link |= (1<<IpFamilyV6);
                                usbnet_link_change(link, profile);
                            }
                            
                            if ((profile->enable_ipv4 && IPv4ConnectionStatus ==  QWDS_PKT_DATA_DISCONNECTED)
                                || (profile->enable_ipv6 && IPv6ConnectionStatus ==  QWDS_PKT_DATA_DISCONNECTED)) {
                                send_signo_to_main(SIG_EVENT_START);
                            }
                        break;

                        case SIG_EVENT_STOP:
                            if (profile->enable_ipv4 && IPv4ConnectionStatus ==  QWDS_PKT_DATA_CONNECTED) {
                                request_ops->requestDeactivateDefaultPDP(profile, IpFamilyV4);
                            }
                            if (profile->enable_ipv6 && IPv6ConnectionStatus ==  QWDS_PKT_DATA_CONNECTED) {
                                if (profile->enable_ipv4 && profile->request_ops != &qmi_request_ops) {
                                    
                                }
                                else {
                                    request_ops->requestDeactivateDefaultPDP(profile, IpFamilyV6);
                                }
                            }
                            usbnet_link_change(0, profile);
                            if (profile->qmi_ops->deinit)
                                profile->qmi_ops->deinit();
                            main_send_event_to_qmidevice(RIL_REQUEST_QUIT);
                            goto __main_quit;
                        break;

                        default:
                        break;
                    }
                }
            }

            if (fd == qmidevice_control_fd[0]) {
                if (read(fd, &triger_event, sizeof(triger_event)) == sizeof(triger_event)) {
                    switch (triger_event) {
                        case RIL_INDICATE_DEVICE_DISCONNECTED:
                            usbnet_link_change(0, profile);                            
                            goto __main_quit;
                        break;

                        case RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED:
                            request_ops->requestRegistrationState(&PSAttachedState);
                            if (PSAttachedState == 1) {
                                if ((profile->enable_ipv4 && IPv4ConnectionStatus ==  QWDS_PKT_DATA_DISCONNECTED)
                                    || (profile->enable_ipv6 && IPv6ConnectionStatus ==  QWDS_PKT_DATA_DISCONNECTED)) {
                                    send_signo_to_main(SIG_EVENT_START);
                                }
                             } else {
                                SetupCallAllowTime = clock_msec();
                             }
                        break;

                        case RIL_UNSOL_DATA_CALL_LIST_CHANGED:
                            if (IPv4ConnectionStatus ==  QWDS_PKT_DATA_CONNECTED || IPv6ConnectionStatus ==  QWDS_PKT_DATA_CONNECTED) {
                                SetupCallAllowTime = clock_msec() + 1000; //from connect -> disconnect, do not re-dail immediately, wait network stable
                            }
                            send_signo_to_main(SIG_EVENT_CHECK);
                        break;

                        case MODEM_REPORT_RESET_EVENT:
                        {
                            unsigned int time_to_wait = 20;
                            unsigned int time_expired = 0;
                            dbg_time("main recv MODEM RESET SIGNAL");
                            dbg_time("quit QMI thread and wait %ds and try to restart", time_to_wait);
                            main_send_event_to_qmidevice(RIL_REQUEST_QUIT);
                            /** NOTICE
                             * DO NOT CALL usbnet_link_change(0, profile) DIRECTLLY
                             * for, the modem may go into wrong state(only ttyUSB0 left) and wont go back
                             */
                            usbnet_link_state(0);
                            /* close FDs, for we want restart. */
                            close(signal_control_fd[0]);
                            close(signal_control_fd[1]);
                            close(qmidevice_control_fd[0]);
                            close(qmidevice_control_fd[1]);
                            while (time_expired++ < time_to_wait) {
                                sleep(1);
                                char qmidev[64] = {'\0'};
                                snprintf(qmidev, sizeof(qmidev), "/dev/bus/usb/%03d/%03d", profile->usb_dev.busnum, profile->usb_dev.devnum);
                                if (access(qmidev, F_OK)) {
                                    dbg_time("whoo, fatal error info, qmi device node disappeared!!! cannot continue!\n");
                                    goto __main_quit;
                                }
                            }
                            dbg_time("main try do restart");
                            goto __main_loop;
                        }
                    	case RIL_UNSOL_LOOPBACK_CONFIG_IND:
                        {
                        	QMI_WDA_SET_LOOPBACK_CONFIG_IND_MSG SetLoopBackInd;
                        	if (read(fd, &SetLoopBackInd, sizeof(SetLoopBackInd)) == sizeof(SetLoopBackInd)) {
                            	profile->loopback_state = SetLoopBackInd.loopback_state.TLVVaule;
                            	profile->replication_factor = le32_to_cpu(SetLoopBackInd.replication_factor.TLVVaule);
                            	dbg_time("SetLoopBackInd: loopback_state=%d, replication_factor=%u",
                                	profile->loopback_state, profile->replication_factor);
                            	if (profile->loopback_state)
                                	send_signo_to_main(SIG_EVENT_START);
                            }
                        }
                    	break;
                        default:
                        break;
                    }
                }
            }
        }
    }

__main_quit:
    usbnet_link_change(0, profile);
    if (gQmiThreadID && pthread_join(gQmiThreadID, NULL)) {
        dbg_time("%s Error joining to listener thread (%s)", __func__, strerror(errno));
    }
    close(signal_control_fd[0]);
    close(signal_control_fd[1]);
    close(qmidevice_control_fd[0]);
    close(qmidevice_control_fd[1]);
    dbg_time("%s exit", __func__);

    return 0;
}

#define has_more_argv() ((opt < argc) && (argv[opt][0] != '-'))
int main(int argc, char *argv[])
{
    int opt = 1;
    const char *usbmon_logfile = NULL;
    PROFILE_T profile;
    int ret = -1;

    dbg_time("Quectel_QConnectManager_Linux_V1.6.0.24");
    memset(&profile, 0x00, sizeof(profile));
    profile.pdp = CONFIG_DEFAULT_PDP;

    if (!strcmp(argv[argc-1], "&"))
        argc--;

    opt = 1;
    while  (opt < argc) {
        if (argv[opt][0] != '-')
            return usage(argv[0]);

        switch (argv[opt++][1])
        {
            case 's':
                profile.apn = profile.user = profile.password = "";
                if (has_more_argv())
                    profile.apn = argv[opt++];
                if (has_more_argv())
                    profile.user = argv[opt++];
                if (has_more_argv())
                {
                    profile.password = argv[opt++];
                    if (profile.password && profile.password[0])
                        profile.auth = 2; //default chap, customers may miss auth
                }
                if (has_more_argv()) {
                    const char *auth = argv[opt++];

                    if (!strcmp(auth, "0") || !strcasecmp(auth, "none")) {
                        profile.auth = 0;
                    } else if (!strcmp(auth, "1") || !strcasecmp(auth, "pap")) {
                        profile.auth = 1;
                    } else if (!strcmp(auth, "2") || !strcasecmp(auth, "chap")) {
                        profile.auth = 2;
                    } else {
                        dbg_time("unknow auth '%s'", auth);
                        return usage(argv[0]);
                    }
                }
            break;

            case 'p':
                if (has_more_argv()) {
                    const char *arg = argv[opt++];

                    if (!strcmp(arg, QUECTEL_QMI_PROXY) || !strcmp(arg, QUECTEL_MBIM_PROXY)
                        || !strcmp(arg, LIBQMI_PROXY) || !strcmp(arg, LIBMBIM_PROXY)) {
                        strncpy(profile.proxy, arg, sizeof(profile.proxy));
                    }
                    else if ((999 < atoi(arg)) && (atoi(arg) < 10000)) {
                        profile.pincode = argv[opt++];
                    } else {
                        dbg_time("unknow -p '%s'", arg);
                        return usage(argv[0]);
                    }
                }
            break;

            case 'm':
                if (has_more_argv())
                    profile.muxid = argv[opt++][0] - '0' + 0x80;
            break;

            case 'n':
                if (has_more_argv())
                    profile.pdp = argv[opt++][0] - '0';
            break;

            case 'c':
                if (has_more_argv())
                {
                  apnConfigfile = argv[opt++];
                  dbg_time("[%s]: apnConfigfile is %s", __func__, apnConfigfile);
                }
            break;

            case 'f':
                if (has_more_argv())
                {
                    const char * filename = argv[opt++];
                    logfilefp = fopen(filename, "a+");
                    if (!logfilefp) {
                        dbg_time("Fail to open %s, errno: %d(%s)", filename, errno, strerror(errno));
                     }
                }
            break;

            case 'i':
                if (has_more_argv())
                    strncpy(profile.usbnet_adapter, argv[opt++], sizeof(profile.usbnet_adapter));
            break;

            case 'v':
                debug_qmi = 1;
            break;

            case 'l':
            	if (has_more_argv()) {
                    profile.replication_factor = atoi(argv[opt++]);
                	if (profile.replication_factor > 0)
                    	profile.loopback_state = 1;
                }
            break;

            case '4':
                profile.enable_ipv4 = 1; 
            break;

            case '6':
                profile.enable_ipv6 = 1;
            break;

            case 'u':
                if (has_more_argv()) {
                    usbmon_logfile = argv[opt++];
                }
            break;

            case 'b':
                profile.enable_bridge = 1;
            break;
			
            case 'k':
                if (has_more_argv()) {
                    return kill_data_call_pdp(argv[opt++][0] - '0', argv[0]);
                }
                break;
            
            default:
                return usage(argv[0]);
            break;
        }
    }

    if (profile.enable_ipv4 != 1 && profile.enable_ipv6 != 1) { // default enable IPv4
        profile.enable_ipv4 = 1;
    }

    if (!(profile.qmichannel[0]) || !(profile.usbnet_adapter[0])) {
        char qmichannel[32+1] = {'\0'};
        char usbnet_adapter[32+1] = {'\0'};

        if (profile.usbnet_adapter[0])
            strncpy(usbnet_adapter, profile.usbnet_adapter, sizeof(usbnet_adapter));
        
        if (qmidevice_detect(qmichannel, usbnet_adapter, sizeof(qmichannel), &profile)) {
        	profile.hardware_interface = HARDWARE_USB;
        }
        else if (mhidevice_detect(qmichannel, usbnet_adapter, &profile)) {
            profile.hardware_interface = HARDWARE_PCIE;
        }
        else {
            dbg_time("qmidevice_detect failed");
            goto error;
        }
 
        strncpy(profile.qmichannel, qmichannel, sizeof(profile.qmichannel));
        strncpy(profile.usbnet_adapter, usbnet_adapter, sizeof(profile.usbnet_adapter));
        ql_get_netcard_driver_info(profile.usbnet_adapter);

        if ((profile.hardware_interface == HARDWARE_USB) && usbmon_logfile)
            ql_capture_usbmon_log(&profile, usbmon_logfile);

        if (profile.hardware_interface == HARDWARE_USB) {
            profile.software_interface = get_driver_type(&profile);
        }
    }
  
    ql_qmap_mode_detect(&profile);

    if (profile.software_interface == SOFTWARE_MBIM) {
        dbg_time("Modem works in MBIM mode");
        profile.request_ops = &mbim_request_ops;
        profile.qmi_ops = &mbim_dev_ops;
        ret = qmi_main(&profile);
    }
    else if (profile.software_interface == SOFTWARE_QMI) {
        dbg_time("Modem works in QMI mode");
        profile.request_ops = &qmi_request_ops;
        if (qmidev_is_gobinet(profile.qmichannel))
            profile.qmi_ops = &gobi_qmidev_ops;
        else
            profile.qmi_ops = &qmiwwan_qmidev_ops;
        qmidev_send = profile.qmi_ops->send;
        ret = qmi_main(&profile);
    }
    else if (profile.software_interface == SOFTWARE_ECM_RNDIS_NCM) {
        dbg_time("Modem works in ECM_RNDIS_NCM mode");
        profile.request_ops = &atc_request_ops;
        profile.qmi_ops = &atc_dev_ops;
        ret = qmi_main(&profile);
    }
    else {
        dbg_time("unsupport software_interface %d", profile.software_interface);
    }

    ql_stop_usbmon_log(&profile);
    if (logfilefp)
    fclose(logfilefp);

error:

    return ret;
}
