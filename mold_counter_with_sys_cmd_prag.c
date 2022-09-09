#include <time.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <dal/adi_application_interface.h>
#include <pthread.h>

#define WINDOW_SZ 100
#define RECORD_QSZ 20
#define TX_FREQ_SEC (1*60)
#define SAVE_FREQ_SEC (15*60)
#define DEBUG_LOG_LEVEL_DEFAULT 0
#define DEBUG_LOG_LEVEL_MAX 3
#define FILE_DIR "/home/moldcounter/meta/"
#define FILE_META "/home/moldcounter/meta/.meta"
#define FILE_WIN "/home/moldcounter/meta/.win"
#define FILE_REC "/home/moldcounter/meta/.rec"
#define FILE_PUBLISH "/home/moldcounter/meta/.pub"
#define KBUS_MAINPRIO 40
#define TOPIC "/prostream/molddevice002/raw"
#define HOST "a314ck58o90ho8-ats.iot.us-east-2.amazonaws.com"
#define PORT "8883"
#define CERT "/home/moldcounter/certs/device-chain.cert" 
#define KEY "/home/moldcounter/certs/device.key" 
#define CAFILE "/home/moldcounter/certs/AmazonRootCA1.pem"

typedef enum
{
    TYPE_META = 1,
    TYPE_WIN = 2,
    TYPE_REC = 3
} FILE_TYPE;

typedef enum
{
    EVENT_OFF = 0,
    EVENT_PRESSED = 1
} EVENT;

typedef struct
{
   uint32_t window_sz;
   uint32_t rec_qsz;
   uint32_t tx_freq_sec;
   uint32_t save_freq_sec;
   uint32_t dbg_log_level;
} METADATA;

typedef struct
{
   uint32_t count;
   uint32_t s_avg;
   uint32_t l_avg;
} RECORD;

typedef struct
{
	uint32_t sz;
	RECORD arr[RECORD_QSZ];
} PACKET;

static METADATA g_meta;
static RECORD g_rec;
static RECORD g_rec_queue[RECORD_QSZ];
static PACKET g_packet;
static uint32_t g_index;
static uint32_t g_total;
static uint32_t w_rec_idx;
static uint32_t r_rec_idx;
static uint32_t w_rec_cnt;
static uint32_t r_rec_cnt;
static time_t g_window[WINDOW_SZ];
static time_t g_sum;
static time_t g_last;
static char g_cmd[500];

void platform_init()
{
   memset(g_window, 0, sizeof(g_window));
   memset(g_rec_queue, 0, sizeof(g_rec_queue));
   memset(&g_rec, 0, sizeof(g_rec));
   memset(&g_packet, 0, sizeof(g_packet));
   memset(g_cmd, 0, sizeof(g_cmd));
   g_meta.window_sz = WINDOW_SZ;
   g_meta.rec_qsz = RECORD_QSZ;
   g_meta.tx_freq_sec = TX_FREQ_SEC;
   g_meta.save_freq_sec = SAVE_FREQ_SEC;
   g_meta.dbg_log_level = DEBUG_LOG_LEVEL_DEFAULT;
   g_index = 0;
   g_sum = 0;
   g_total = 0;
   g_last = 0;
   w_rec_idx = 0;
   r_rec_idx = 0;
   w_rec_cnt = 0;
   r_rec_cnt = 0;
   sprintf(g_cmd, "mosquitto_pub --cert %s --key %s --cafile %s -h %s -p %s -t %s -f %s",
                    CERT, KEY, CAFILE, HOST, PORT, TOPIC, FILE_PUBLISH);
   
   struct stat st = {0};

   if(stat(FILE_DIR, &st) == -1)
   {
        system("mkdir -p /home/moldcounter/meta/");
   }
}

void window_update(const time_t value)
{
   if(g_meta.dbg_log_level >= 2)
        printf("window_update called\n");
        
   g_sum -= g_window[g_index];  
   g_sum += value;
   g_window[g_index] = value;
   g_index++;
   g_index %= g_meta.window_sz;
   g_total++;
   g_last = value;
}

void queue_add(RECORD const * const pData)
{
   if(g_meta.dbg_log_level >= 2)
        printf("queue_add called\n");
        
    w_rec_cnt++;
    memcpy(&g_rec_queue[w_rec_idx], pData, sizeof(RECORD));
    w_rec_idx++;
    w_rec_idx %= g_meta.rec_qsz;
}

bool queue_get(RECORD * const pData)
{
    if(r_rec_cnt < w_rec_cnt)
    {
        if(g_meta.dbg_log_level >= 2)
            printf("queue_get called\n");
    
        r_rec_cnt++;
        memcpy(pData, &g_rec_queue[r_rec_idx], sizeof(RECORD));
        r_rec_idx++;
        r_rec_idx %= g_meta.rec_qsz;
        
        return true;
    }
    
    return false;
}

void get_rec(RECORD * const pData)
{
    if(g_meta.dbg_log_level >= 2)
            printf("get_rec called\n");
            
    pData->count = g_total;
    pData->l_avg = (uint32_t)g_last;
    pData->s_avg = 0;
        
    if(g_total >= g_meta.window_sz)
    {
        pData->s_avg = (uint32_t)(g_sum/g_meta.window_sz);
    }
}

uint8_t get_io_data(tDeviceId * const pDev, uint32_t * const pTid, tApplicationDeviceInterface * pAdi)
{
	uint8_t data;
    pAdi->ReadStart(*pDev, *pTid);
    pAdi->ReadBytes(*pDev, *pTid, 0, 1, (uint8_t *) &data);
    pAdi->ReadEnd(*pDev, *pTid);
    
    if(g_meta.dbg_log_level >= 3)
	    printf("IO data=%d\t", data);
	
	return data;
}

bool check_event(uint8_t data, EVENT evt)
{
    uint8_t ref = 0x1;
    ref <<= evt;
    if((data & ref) > 0)
    {
        return true ;
    }
    
    return false;
}

void file_write(FILE_TYPE type)
{
    FILE *fp = NULL;
    
    if(type == TYPE_META)
    {
        fp = fopen(FILE_META, "w");
    }
    else if(type == TYPE_WIN)
    {
        fp = fopen(FILE_WIN, "w");
    }
    else if(type == TYPE_REC)
    {
        fp = fopen(FILE_REC, "w");
    }
    
    if(fp != NULL)
    {
        switch(type)
        {
            case TYPE_META:
            {  
                fwrite(&g_meta, 1, sizeof(METADATA), fp);
                break;
            }
            case TYPE_WIN:
            {
                fwrite(g_window, 1, sizeof(g_window), fp);
                break;
            }
            case TYPE_REC:
            {
                fwrite(&g_rec, 1, sizeof(RECORD), fp);
                break;
            }
            default:
            break;
        }
        fclose(fp);
    }
    else
    {
        if(g_meta.dbg_log_level >= 2)
            printf("Failed file_write:%d\n", type);
    }
}

void file_read(FILE_TYPE type)
{
    FILE *fp = NULL;
    
    if(type == TYPE_META)
    {
        fp = fopen(FILE_META, "r");
    }
    else if(type == TYPE_WIN)
    {
        fp = fopen(FILE_WIN, "r");
    }
    else if(type == TYPE_REC)
    {
        fp = fopen(FILE_REC, "r");
    }
    
    if(fp != NULL)
    {
        fseek(fp, 0L, SEEK_END);
        ssize_t sz = ftell(fp);
        if(sz > 0)
        {
            rewind(fp);
            
            switch(type)
            {
                case TYPE_META:
                {
                    fread(&g_meta, 1, sizeof(METADATA), fp);
                    break;
                }
                case TYPE_WIN:
                {
                    fread(g_window, 1, sizeof(g_window), fp);
                    break;
                }
                case TYPE_REC:
                {
                    fread(&g_rec, 1, sizeof(RECORD), fp);
                    break;
                }
                default:
                break;
            }
        }
        fclose(fp);
    }
    else
    {
        if(g_meta.dbg_log_level >= 2)
            printf("Failed file_read:%d\n", type);
    }
}

void print_op_data(void)
{
    printf("\n************************************************************************************************************\n");
	printf("Meta - WindowSz:%u, RecQueueSz:%u, TxPeriod:%u, SavePeriod:%u, DebugLevel:%u\n",
	            g_meta.window_sz, g_meta.rec_qsz, g_meta.tx_freq_sec, g_meta.save_freq_sec, g_meta.dbg_log_level);
	printf("Last Record - TotalCount:%u, LastCycleTime:%u, AvgCyclesTime:%u\n", g_rec.count, g_rec.l_avg, g_rec.s_avg);
	printf("Window Values:\n");
	for(uint32_t i = 0; i < g_meta.window_sz; i++)
	{
	    printf("%ld\t", g_window[i]);
	}
	printf("\n************************************************************************************************************\n");
}
                
void handle_power_down(void)
{
    get_rec(&g_rec);
    file_write(TYPE_REC);
    file_write(TYPE_WIN);
       
    if(g_meta.dbg_log_level >= 1)
    {
        printf("PowerDown initiated!!!\n");
        print_op_data();
	}
}

void publish_data(PACKET * const pData)
{
    FILE *fp = fopen(FILE_PUBLISH, "w");
    
    if(fp != NULL)
    {
        fwrite(pData, 1, sizeof(PACKET), fp);
        fclose(fp);
        system(g_cmd);
        
        if(g_meta.dbg_log_level >= 1)
        {
            printf("Published Records:%u\n", pData->sz);
            for(uint32_t i = 0; i < pData->sz; i++)
            {
                printf("Record#%u - TotalCount:%u, LastCycleTime:%u, AvgCyclesTime:%u\n",
                        (i+1), pData->arr[i].count, pData->arr[i].l_avg, pData->arr[i].s_avg);
            }
        }
    }
    else
    {
        if(g_meta.dbg_log_level >= 2)
            printf("Failed to create publish file\n");
    }
}

void* event_handler(void * ptr)
{
    time_t t_cur = time(NULL);
	time_t t_lastTx = t_cur;
	time_t t_lastRom = t_cur;
    RECORD t_rec;
    
    while(1)
    {
        t_cur = time(NULL);
        
        if((t_cur - t_lastTx) >= g_meta.tx_freq_sec)
	    {
	        if(g_meta.dbg_log_level >= 2)
                    printf("Triggered Async mechanism!!!\n");
                    
	        t_lastTx = t_cur;
	        memset(&g_packet, 0, sizeof(PACKET));
	        uint32_t idx = 0;
            while(queue_get(&t_rec) == true)
            {
                memcpy(&g_packet.arr[idx], &t_rec, sizeof(RECORD));
                idx++;
                
                if(g_meta.dbg_log_level >= 2)
                    printf("Async prepared record#%u\n", idx);
            }
                    
            if(idx > 0)
            {
                g_packet.sz = idx;
                publish_data(&g_packet);
            }
	    }
		
	    if((t_cur - t_lastRom) >= g_meta.save_freq_sec)
	    {
	        if(g_meta.dbg_log_level >= 1)
		        printf("Data saved to ROM\n");
		            
	        t_lastRom = t_cur;
	        get_rec(&g_rec);
	        file_write(TYPE_REC);
	        file_write(TYPE_WIN);
	    }
	
	    sleep(1);
	}
}

void print_help(void)
{
    printf("-w : averaging window size, default:%d, max:%d\n", WINDOW_SZ, WINDOW_SZ);
    printf("-q : record queue size, default:%d, max:%d\n", RECORD_QSZ, RECORD_QSZ);
    printf("-p : record(s) transmit period (sec), default:%d\n", TX_FREQ_SEC);
    printf("    Note: depth of 'q' should be considered related to 'p'\n");
    printf("-s : record(s) save to ROM period (sec), default:%d\n", SAVE_FREQ_SEC);
    printf("-d : debug log level from 0(lower) to 3(higher), default:%d\n", DEBUG_LOG_LEVEL_DEFAULT);
}

void handle_cmdline(int argc, char *argv[])
{
    int32_t opt;
    
    while((opt = getopt(argc, argv, "hw:q:p:s:d:")) != -1) 
    { 
        switch(opt) 
        {
            case 'h':
            {
                print_help();
                exit(0);
            } 
            case 'w':
            { 
                uint32_t val = atoi(optarg);
                if(val < WINDOW_SZ)
                {
                    g_meta.window_sz = val;
                    file_write(TYPE_META);
                }
                break;
            }
            case 'q':
            { 
                uint32_t val = atoi(optarg);
                if(val < RECORD_QSZ)
                {
                    g_meta.rec_qsz = val;
                    file_write(TYPE_META);
                }
                break;
            } 
            case 'p':
            {
                g_meta.tx_freq_sec = atoi(optarg);
                file_write(TYPE_META);
                break; 
            }
            case 's':
            { 
                g_meta.save_freq_sec = atoi(optarg);
                file_write(TYPE_META);
                break; 
            }
            case 'd':
            {
                uint32_t val = atoi(optarg);
                if(val <= DEBUG_LOG_LEVEL_MAX)
                {
                    g_meta.dbg_log_level = val;
                    file_write(TYPE_META);
                }
                break; 
            }
            case '?':
            { 
                printf("unknown option, try -h for help\n");
                exit(0);
            } 
        } 
    }
}

int main(int argc, char *argv[])
{
    tDeviceInfo deviceList[10];         
    tDeviceId kbusDeviceId;             
    tApplicationDeviceInterface * adi;  
	tApplicationStateChangedEvent event;
	size_t nrDevicesFound;              
    size_t nrKbusFound;               
    struct sched_param s_param;
	time_t t_start = 0;
	time_t t_end = 0;
	pthread_t th;
	bool in_process = false;
	uint32_t taskId = 0; 
	uint32_t retval = 0;
	int32_t ret;
    
    platform_init();
    file_read(TYPE_META);
	file_read(TYPE_WIN);
	file_read(TYPE_REC);
    handle_cmdline(argc, argv);
    
    adi = adi_GetApplicationInterface();
    adi->Init();
    adi->ScanDevices();
    adi->GetDeviceList(sizeof(deviceList), deviceList, &nrDevicesFound);

    nrKbusFound = -1;
    for (uint32_t i = 0; i < nrDevicesFound; ++i)
    {
        if (strcmp(deviceList[i].DeviceName, "libpackbus") == 0)
        {
            nrKbusFound = i;
            
            if(g_meta.dbg_log_level >= 2)
                printf("KBUS device found as device %i\n", i);
        }
    }

    if (nrKbusFound == -1)
    {
        if(g_meta.dbg_log_level >= 1)
            printf("No KBUS device found\n");
            
        adi->Exit(); 
        return -1;
    }

    s_param.sched_priority = KBUS_MAINPRIO;
    sched_setscheduler(0, SCHED_FIFO, &s_param);
    kbusDeviceId = deviceList[nrKbusFound].DeviceId;
    if (adi->OpenDevice(kbusDeviceId) != DAL_SUCCESS)
    {
        if(g_meta.dbg_log_level >= 1)
            printf("Kbus device open failed\n");
            
        adi->Exit();
        return -2;
    }

    event.State = ApplicationState_Running;
    if (adi->ApplicationStateChanged(event) != DAL_SUCCESS)
    {
        if(g_meta.dbg_log_level >= 1)
            printf("Set application state to 'Running' failed\n");
            
        adi->CloseDevice(kbusDeviceId);
        adi->Exit();
        return -3;
    }
	
	ret = pthread_create(&th, NULL, event_handler, NULL);
	if(ret != 0)
	{
	    if(g_meta.dbg_log_level >= 1)
	        printf("\n ERROR: return code from pthread_create is %d \n", ret);
	}
	
	if(g_meta.dbg_log_level >= 1)
	{
	    printf("PowerUp settings!!!\n");
	    print_op_data();
	}
	
	while(1)
	{
		usleep(10000);
		adi->WatchdogTrigger();

	    if (adi->CallDeviceSpecificFunction("libpackbus_Push", &retval) != DAL_SUCCESS)
        {
            if(g_meta.dbg_log_level >= 1)
                printf("CallDeviceSpecificFunction failed\n");
                
            adi->CloseDevice(kbusDeviceId);
            adi->Exit();
            return -8;
        }

        if (retval != DAL_SUCCESS)
        {
            if(g_meta.dbg_log_level >= 1)
                printf("Function 'libpackbus_Push' failed\n");
                
            adi->CloseDevice(kbusDeviceId);
            adi->Exit();
            return -9;
        }
		
		uint8_t io_data = get_io_data(&kbusDeviceId, &taskId, adi);
		
		if(check_event(io_data, EVENT_PRESSED) == true)
		{
			if(in_process == false)
			{
			    if(g_meta.dbg_log_level >= 3)
				    printf("Key pressed\n");
				
				in_process = true;
				t_start = time(NULL);
				t_end = t_start;
			}
		}
		else
		{
			if(in_process == true)
			{
			    if(g_meta.dbg_log_level >= 3)
				    printf("Key released\n");
				
				in_process = false;
				t_end = time(NULL);
				window_update(t_end - t_start);
				get_rec(&g_rec);
				queue_add(&g_rec);
			}
		}
		
		if(check_event(io_data, EVENT_OFF) == true)
		{
		    handle_power_down();
		}
	}

    adi->CloseDevice(kbusDeviceId);
    adi->Exit();
    pthread_join(th, NULL);
    
    return 0;
}
