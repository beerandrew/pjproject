#include "lws_config.h"
#include "lib/json.h"
#include "lib/json.c"
#include <pjsua-lib/pjsua.h>
#include <libwebsockets.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <libwebsockets.h>
#include <pthread.h>
#include "lib/vector.h"
#include "lib/pipe.h"
#include "lib/pipe.c"
#include <curl/curl.h>

#define THIS_FILE	"APP"

#define SIP_DOMAIN	"18.224.233.81"
#define SIP_USER	"1111"
#define SIP_PASSWD	"QAb+yyt6MnjiqMrS7xy3"
#define WAV_FILE	"auddemo.wav"
#define MAX_TRY_CNT 5
#define CCCC		5

char* str_copy(char *str) {
	char *copied = malloc(sizeof(char)*(strlen(str)+1));
	strcpy(copied, str);
	return copied;
}

pthread_mutex_t ws_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t write_ext_mutex = PTHREAD_MUTEX_INITIALIZER;

static int bExit;
int didDestroy = 0;

struct profile_info {
	char phone[20];
	char phonenumbers[255];
	char name[200];
	int number_commands;
	char user_input_list[100][1000];
	int cmdLen[100];
	char cmd[100][10][100];
	int thread_cnt;
	int finished_thread_cnt;
	struct call_to_profile_with_number **call_queue;
};

char binary_buf[1000000];
struct call_info {
	int isProfileI;
	int disconnected;
	struct lws *wsiTest; // WebSocket interface
	pthread_t ws_thread_id;
	char globalBuf[1000000];
	int bufferSize;
	// pjmedia_port* media_port;
	pjsua_call_id call_id;
	pjsua_recorder_id rec_id;
	pjsua_conf_port_id rec_slot;
	char transcription[1000];
	struct profile_info *pi;
	int ci;//profile thread indx
	int prv_ran_cmd_id;
	char prv_ran_cmd_param[100];
	int done_ext;
	int tried_cnt;
	pipe_t* transcriptions;
	char callerId[20];
	bool sending;
	bool shouldSendStop;

	// mutex
	pthread_mutex_t ws_buf_mutex;
};

struct call_dtmf_data
{
   pj_pool_t          *pool;
   pjmedia_port       *tonegen;
   pjsua_conf_port_id  toneslot;
};

struct call_to_profile_with_number {
	struct profile_info *pi;
	int number;
	int tried_cnt;
	char callerId[20];
};

struct pjsua_player_eof_data
{
    pj_pool_t          *pool;
    pjsua_player_id player_id;
};

struct call_info **current_calls;
// struct lws *wsiTest; // WebSocket interface1
// char *globalBuf = NULL;//1
// int bufferSize = 0;//1
// pjsua_call_id current_call_id;//1
// pjsua_recorder_id current_rec_id = PJSUA_INVALID_ID;//1
// pjsua_conf_port_id current_rec_slot = PJSUA_INVALID_ID;//1
// char current_transcription[1000]="";//1
struct profile_info current_profile_info;
char *current_profile_name = NULL; //it is used for "profile -I" and "profile -S"
char response_list[100][1000];
char user_input_list[100][1000];
int user_input_cnt = 0;
pjsua_acc_id *shared_acc_id = NULL;

pjmedia_port *player_media_port;

void call_play_digit(pjsua_call_id call_id, const char *digits);
static PJ_DEF(pj_status_t) on_pjsua_wav_file_end_callback(pjmedia_port* media_port, void* args);
void *send_thread_func(void *vargp);

void on_dial_command(struct call_info *this_call_info, char *dial_number) {
	this_call_info->sending = 1;
	this_call_info->shouldSendStop = 1;
	PJ_LOG(1, (THIS_FILE, "Call %d: Dial %s\n", this_call_info->call_id, dial_number));
	call_play_digit(this_call_info->call_id, dial_number);
}
void on_speak_command(char *to_speak, pjsua_call_id call_id) {
	PJ_LOG(1, (THIS_FILE, "<<**>> on_speak_command started"));
	PJ_LOG(1, (THIS_FILE, "Speak %s call_id: %d", to_speak, call_id));
	
	char wavfile[200];
    sprintf(wavfile, "%s.wav", to_speak);

	if( access( wavfile, F_OK ) == -1 ) {
		download_wav(to_speak);
	}

	pj_status_t status;

	// Send starts
	pjsua_call_info ci;
	pjsua_call_get_info(call_id, &ci);

	pj_pool_t *pool = NULL;
	pjsua_player_id player_id = PJSUA_INVALID_ID;
	pjsua_conf_port_id player_slot = PJSUA_INVALID_ID;
	status = PJ_SUCCESS;

	const pj_str_t filename = pj_str(wavfile);
	// ( const pj_str_t *filename,
	// 	 unsigned options,
	// 	 pjsua_player_id *p_id)
	status = pjsua_player_create(&filename, PJMEDIA_FILE_NO_LOOP, &player_id);
	if (status != PJ_SUCCESS)
		goto on_return;

	pool = pjsua_pool_create("player%p", 512, 512);
	struct pjsua_player_eof_data *eof_data = PJ_POOL_ZALLOC_T(pool, struct pjsua_player_eof_data);
	eof_data->pool = pool;
	eof_data->player_id = player_id;

	status = pjsua_player_get_port(player_id, &player_media_port);
	if (status != PJ_SUCCESS)
		goto on_return;
	pjmedia_wav_player_set_eof_cb(player_media_port, eof_data, &on_pjsua_wav_file_end_callback);

	player_slot = pjsua_player_get_conf_port(player_id);
	
	pjsua_player_set_pos(player_id, 0);
	
	status = pjsua_conf_connect(player_slot, pjsua_call_get_conf_port(call_id));
	if (status != PJ_SUCCESS)
		goto on_return;

	PJ_LOG(1, (THIS_FILE, "<<**>> on_speak_command ended"));
	return;

on_return:
	if (player_slot != PJSUA_INVALID_ID)
	pjsua_conf_disconnect(player_slot, ci.conf_slot);
	if (player_id != PJSUA_INVALID_ID)
	pjsua_player_destroy(player_id);
	if (pool)
	pj_pool_release(pool);
	PJ_LOG(1, (THIS_FILE, "<<**>> on_speak_command ended"));
}

int find_index_from_call_id(pjsua_call_id call_id) {
	int i;
	for(i = 0; i < vector_size(current_calls); i ++) {
		if (current_calls[i]->call_id == call_id) {
			return i;
		}
	}
	// printf("<<**>> find_index_from_call_id not found by call_id %d\n", call_id);
	return -1;
}
int find_index_from_rec_id(unsigned rec_id) {
	int i;
	for(i = 0; i < vector_size(current_calls); i ++) {
		if (current_calls[i]->rec_id == rec_id) {
			return i;
		}
	}
	// printf("<<**>> find_index_from_call_id not found by call_id %d\n", call_id);
	return -1;
}

int find_index_from_websocket(	struct lws *wsiTest) {
	int i;
	for(i = 0; i < vector_size(current_calls); i ++) {
		if (current_calls[i]->wsiTest == wsiTest) {
			return i;
		}
	}
	// printf("<<**>> find_index_from_websocket not found by wsiTest %x\n", wsiTest);
	// for(i = 0; i < vector_size(current_calls); i ++) {
	// 	printf("check wsiTest (%x ?? %x)\n", current_calls[i]->wsiTest, wsiTest);
	// }
	return -1;
}
int find_index_profile_insert() {
	int i;
	for(i = 0; i < vector_size(current_calls); i ++) {
		if (current_calls[i]->isProfileI == 1) {
			return i;
		}
	}
	
	return -1;	
}
void init_call_info(struct call_info *ci) {
	ci->isProfileI = 0;
	ci->disconnected = 0;
	ci->wsiTest = NULL;// malloc( sizeof(struct lws) );
	ci->ws_thread_id = NULL;
	ci->pi = NULL;
	ci->bufferSize = 0;
	ci->call_id = PJSUA_INVALID_ID;
	ci->rec_id = PJSUA_INVALID_ID;
	ci->rec_slot = PJSUA_INVALID_ID;
	ci->transcription[0] = '\0';
	ci->done_ext = 0;
	ci->transcriptions = pipe_new(sizeof(char) * 1000, 0);
	ci->sending = 0;
	ci->shouldSendStop = 0;
	ci->ci = -1;
	ci->prv_ran_cmd_id = -1;
	ci->tried_cnt = 0;
	pthread_mutex_init ( &ci->ws_buf_mutex, NULL);
}

void on_call_end() {

}
void *make_call_to_profile(void *vargp);

void call_hangup_retry(pjsua_call_id call_id, pjsua_call_info *ci) {
	struct call_info *this_call_info;
	int call_index;
	
	call_index = find_index_from_call_id(call_id);
	if (call_index != -1) {
		this_call_info = current_calls[call_index];
	}

	if (call_index == -1) {
	
		return;
	}

	call_deinit_tonegen(call_id);
	
	if (this_call_info->rec_slot == PJSUA_INVALID_ID) {
	} else {
		if (ci->conf_slot == PJSUA_INVALID_ID) {
		} else {
			pjsua_conf_disconnect(ci->conf_slot, this_call_info->rec_slot);
		}
	}
	// TODO: destroy recorder
	if (this_call_info->rec_id != PJSUA_INVALID_ID)
		pjsua_recorder_destroy(this_call_info->rec_id);
	pthread_mutex_destroy(&this_call_info->ws_buf_mutex);
	pipe_free(this_call_info->transcriptions);
	this_call_info->rec_slot = PJSUA_INVALID_ID;
	this_call_info->call_id = -1;
	this_call_info->wsiTest = NULL;
	struct profile_info *pi = this_call_info->pi;
	if (this_call_info->done_ext > 0) {
		pi->finished_thread_cnt ++;
		// if (pi->finished_thread_cnt == pi->thread_cnt) {
		// 	printf("<<**>> do free of profile_info\n");
		// } else {
		// 	printf("<<**>>Currently finished %d in total %d\n", pi->finished_thread_cnt, pi->thread_cnt);
		// }
	} else if(this_call_info->isProfileI == 0) {
		if (this_call_info->tried_cnt < MAX_TRY_CNT - 1) {
			PJ_LOG(1, (THIS_FILE, "<<**>> restarting call since unexpected transcription received\n"));
			
			struct call_to_profile_with_number *thread_param = malloc(sizeof(struct call_to_profile_with_number));
			// pthread_t make_profile_call_thread_id;
			thread_param->pi = pi;
			thread_param->number = this_call_info->ci;
			thread_param->tried_cnt = this_call_info->tried_cnt + 1;
			strcpy(thread_param->callerId, this_call_info->callerId);

			vector_push_back(pi->call_queue, thread_param);
		} else {
			PJ_LOG(1, (THIS_FILE, "<<**>> tried max_cnt=%d, but did not get result :(\n", MAX_TRY_CNT));

			pthread_mutex_lock(&write_ext_mutex);

			FILE *fp = fopen ("err.txt", "a"); 
			// printf("<err start>--------------<err start>\n");
			// printf("<start ci=%d>--------------<start>\n", this_call_info->ci);
			fprintf(fp, "%d\n", this_call_info->ci);
			// fprintf(fp, "<<**>> tried max_cnt=%d, but did not get result :(\n", MAX_TRY_CNT);
			// fprintf(fp, "<end>--------------<end>\n");
			fclose(fp);

			pthread_mutex_unlock(&write_ext_mutex);

			// pthread_mutex_lock(&call_info_mutex);

			// pi->finished_thread_cnt ++;
			// if (pi->finished_thread_cnt == pi->thread_cnt) {
			// 	printf("<<**>> do free of profile_info in error thread\n");
			// 	free(pi->phone);
			// 	free(pi->name);
			// 	int x = 0, y = 0;
			// 	for (x = 0; x < pi->number_commands; x ++) {
			// 		for (y = 0; y < pi->cmdLen[x]; y ++) {
			// 			free(pi->cmd[x][y]);
			// 		}
			// 		free(pi->cmd[x]);
			// 	}
			// } else {
			// 	printf("<<**>>Currently finished %d in total %d  in error thread\n", pi->finished_thread_cnt, pi->thread_cnt);
			// }

			// pthread_mutex_unlock(&call_info_mutex);
		}
	}

	this_call_info->disconnected = 1;
	vector_erase(current_calls, call_index);

}

/* Callback called by the library when call's state has changed */
static void on_call_state(pjsua_call_id call_id, pjsip_event *e)
{
    pjsua_call_info ci;

    PJ_UNUSED_ARG(e);

    pjsua_call_get_info(call_id, &ci);
    PJ_LOG(1,(THIS_FILE, "Call %d state=%.*s", call_id,
			 (int)ci.state_text.slen,
			 ci.state_text.ptr));

	if (strcmp(ci.state_text.ptr, "DISCONNCTD") == 0) {
		call_hangup_retry(call_id, &ci);
	}
}
pj_status_t	on_putframe(pjmedia_port* port, pjmedia_frame* frame, unsigned rec_id) {
	// printf("<<**>> on_putframe started\n");
	// PJ_LOG(1, (THIS_FILE, "on_putframe started %d", rec_id));
	struct call_info *this_call_info;
	int call_index;
	call_index = find_index_from_rec_id(rec_id);
	if (call_index != -1) {
		this_call_info = current_calls[call_index];
	}

	if (call_index != -1 && this_call_info->call_id != -1) {
		// if (frame->size == 0)
		// 	return 0;
		// printf("<<**>> on_putframe call_index != -1\n");
		// PJ_LOG(1, (THIS_FILE, "<<**>> on_putframe  (threadid: %d, call_id: %d)\n", this_call_info->ws_thread_id, this_call_info->call_id));
		// don't send while sending

		struct call_dtmf_data* cd = (struct call_dtmf_data*) pjsua_call_get_user_data(this_call_info->call_id);
		// if has dtmf 
		if (cd) {
			if (!pjmedia_tonegen_is_busy(cd->tonegen)) {
				call_deinit_tonegen(this_call_info->call_id);
				this_call_info->sending = 0;
				this_call_info->shouldSendStop = 0;
			}
		}
		
		if (this_call_info->sending) {
			return 0;
		}
		pthread_mutex_lock(&this_call_info->ws_buf_mutex);
		 
		// printf("<<**>> b\n");
		memcpy(this_call_info->globalBuf + this_call_info->bufferSize, frame->buf, frame->size);
		this_call_info->bufferSize += frame->size;
		// printf("BUF:%d\n", this_call_info->bufferSize);
		// printf("<<**>> c\n");

		pthread_mutex_unlock(&this_call_info->ws_buf_mutex);

		// if (this_call_info->wsiTest != NULL){
		// 	// printf("<<**>> Issue writable c\n");
		// 	lws_callback_on_writable(this_call_info->wsiTest);
		// }
		// else {
		// 	// printf("disabled lws_callback_on_writable since wsiTest is NULL\n");
		// }
	} else {
		
		// printf("<<**>> on_putframe  (threadid: NULL, call_id: NULL)\n");
	}
	// PJ_LOG(1, (THIS_FILE, "on_putframe ended %d", rec_id));
	
	return 0;
	// printf("<<**>> on_putframe ended\n");
}

void *recorder_thread_func(void *param) {	
	pjsua_call_id this_call_id = *(pjsua_call_id *) param;
	free(param);

	struct call_info *this_call_info;
	int call_index;
	call_index = find_index_from_call_id(this_call_id);
	if (call_index != -1) {
		this_call_info = current_calls[call_index];
	}

	if (call_index == -1) {
		return NULL;
	}

	//TODO: need to check call_id is not changed because of memory
	pj_status_t status;
	pj_thread_desc aPJThreadDesc;
	if (!pj_thread_is_registered()) {
		pj_thread_t *pjThread;
		status = pj_thread_register(NULL, aPJThreadDesc, &pjThread);
		if (status != PJ_SUCCESS) {
		}
	}
	
	pjsua_call_info ci;
    pjsua_call_get_info(this_call_id, &ci);

	pjsua_recorder_id rec_id = PJSUA_INVALID_ID;
	pjsua_conf_port_id rec_slot = PJSUA_INVALID_ID;
	status = PJ_SUCCESS;

	char	    doc_path[PJ_MAXPATH] = {0};
	char wavname[20];
	sprintf(wavname, "%d.wav", this_call_info->call_id);
	const pj_str_t filename = pj_str(wavname);
	status = pjsua_recorder_create(&filename, 0, NULL, -1, 0, &rec_id, on_putframe);


		// pjsua_recorder_get_port(rec_id, &this_call_info->media_port);

	if (status != PJ_SUCCESS)
	{
		goto on_return;

	}

	rec_slot = pjsua_recorder_get_conf_port(rec_id);
	this_call_info->rec_id = rec_id;

	this_call_info->rec_slot = rec_slot;

	status = pjsua_conf_connect(ci.conf_slot, rec_slot);
	if (status != PJ_SUCCESS)
	goto on_return;
	// sleep(60);

	// pjsua_conf_disconnect(ci.conf_slot, rec_slot);
	// rec_slot = PJSUA_INVALID_ID;
	// pjsua_recorder_destroy(rec_id);
	// rec_id = PJSUA_INVALID_ID;
	return NULL;
on_return:
	if (rec_slot != PJSUA_INVALID_ID)
	pjsua_conf_disconnect(ci.conf_slot, rec_slot);
	if (rec_id != PJSUA_INVALID_ID)
	pjsua_recorder_destroy(rec_id);
	PJ_LOG(1, (THIS_FILE, "<<**>> unexpected on_return destroy rec_id"));
	PJ_LOG(1, (THIS_FILE, "<<**>> recorder_thread_func ended"));
    return NULL;
}

/* Callback called by the library when call's media state has changed */
static void on_call_media_state(pjsua_call_id call_id)
{
    pjsua_call_info ci;

    pjsua_call_get_info(call_id, &ci);

    if (ci.media_status == PJSUA_CALL_MEDIA_ACTIVE) {
		pthread_t rec_thread_id; 
		pjsua_call_id *param = malloc(sizeof(pjsua_call_id));
		*param = call_id;
		pthread_create(&rec_thread_id, NULL, recorder_thread_func, param);
	}
}

/* Display error and exit application */
static void error_exit(const char *title, pj_status_t status)
{
    pjsua_perror(THIS_FILE, title, status);
	if (!didDestroy) {
		didDestroy = 1;
    	pjsua_destroy();
	}

    exit(1);
}

static pj_status_t wav_rec_cb(void *user_data, pjmedia_frame *frame)
{
    return pjmedia_port_put_frame((pjmedia_port*)user_data, frame);
}

static int is_ws_created;

static int callback_test(struct lws* wsi, enum lws_callback_reasons reason, void *user, void* in, size_t len);

//////////////////////////////////////////////////////////////////////////

// Escape the loop when a SIGINT signal is received
static void onSigInt(int sig)
{
	bExit = 1;
}

static struct lws_protocols protocols[] = {
	{
		"test-protocol1", // Protocol name
		callback_test,   // Protocol callback
		0,				 // Data size per session (can be left empty)
		2000,			 // Receive buffer size (can be left empty)

	},
	{
		"test-protocol2", // Protocol name
		callback_test,   // Protocol callback
		0,				 // Data size per session (can be left empty)
		2000,			 // Receive buffer size (can be left empty)

	},
	{
		"test-protocol3", // Protocol name
		callback_test,   // Protocol callback
		0,				 // Data size per session (can be left empty)
		2000,			 // Receive buffer size (can be left empty)

	},
	{
		"test-protocol4", // Protocol name
		callback_test,   // Protocol callback
		0,				 // Data size per session (can be left empty)
		2000,			 // Receive buffer size (can be left empty)

	},
	{
		"test-protocol5", // Protocol name
		callback_test,   // Protocol callback
		0,				 // Data size per session (can be left empty)
		2000,			 // Receive buffer size (can be left empty)

	},
	{ NULL, NULL, 0 } // Always needed at the end
};

enum protocolList {
	PROTOCOL_TEST1,
	PROTOCOL_TEST2,
	PROTOCOL_TEST3,
	PROTOCOL_TEST4,
	PROTOCOL_TEST5,
	PROTOCOL_LIST_COUNT // Needed
};
static void print_depth_shift(int depth)
{
	int j;
	for (j=0; j < depth; j++) {
			printf(" ");
	}
}

static void process_value(json_value* value, int depth);

static void process_object(json_value* value, int depth)
{
        int length, x;
        if (value == NULL) {
                return;
        }
        length = value->u.object.length;
        for (x = 0; x < length; x++) {
                print_depth_shift(depth);
                printf("object[%d].name = %s\n", x, value->u.object.values[x].name);
                process_value(value->u.object.values[x].value, depth+1);
        }
}

static void process_array(json_value* value, int depth)
{
        int length, x;
        if (value == NULL) {
                return;
        }
        length = value->u.array.length;
        printf("array\n");
        for (x = 0; x < length; x++) {
                process_value(value->u.array.values[x], depth);
        }
}
static void process_value(json_value* value, int depth)
{
	int j;
	if (value == NULL) {
			return;
	}
	if (value->type != json_object) {
			print_depth_shift(depth);
	}
	switch (value->type) {
		case json_none:
				printf("none\n");
				break;
		case json_object:
				process_object(value, depth+1);
				break;
		case json_array:
				process_array(value, depth+1);
				break;
		case json_integer:
				printf("int: %10" PRId64 "\n", value->u.integer);
				break;
		case json_double:
				printf("double: %f\n", value->u.dbl);
				break;
		case json_string:
				printf("string: %s\n", value->u.string.ptr);
				break;
		case json_boolean:
				printf("bool: %d\n", value->u.boolean);
				break;
	}
} 
char **send_queue = NULL;

// https://stackoverflow.com/questions/907997/physical-distance-between-two-places/908157#908157
int getDifference(char *a, char *b)
{
    // Minimize the amount of storage needed:
    if (strlen(a) > strlen(b))
    {
        // Swap:
        char *x = a;
        a = b;
        b = x;
    }

    // Store only two rows of the matrix, instead of a big one
    int *mat1 = malloc(sizeof(int) * (strlen(a) + 1));
	if (mat1 == NULL) {
		printf("getDifference::malloc failed for mat1");
		return 1000;
	}
    int *mat2 = malloc(sizeof(int) * (strlen(a) + 1));
	if (mat2 == NULL) {
		free(mat1);
		printf("getDifference::malloc failed for mat2");
		return 1000;
	}

    int i;
    int j;

    for (i = 0; i <= strlen(a); i++)
        mat1[i] = i;

    mat2[0] = 1;

    for (j = 1; j <= strlen(b); j++)
    {
        for (i = 1; i <= strlen(a); i++)
        {
            int c = (a[i - 1] == b[j - 1] ? 0 : 1);

            mat2[i] = min(mat1[i - 1] + c, min(mat1[i] + 1, mat2[i - 1] + 1));
        }

        // Swap:
        int *x = mat1;
        mat1 = mat2;
        mat2 = x;

        mat2[0] = mat1[0] + 1;
    }
	int ret = mat1[strlen(a)];
    if (ret < 0) {
        printf("\n");
        for(i = 0; i <= strlen(a); i ++) {
            printf("%d ", mat1[i]);
        }
        printf("\n");
        for(i = 0; i <= strlen(a); i ++) {
            printf("%d ", mat2[i]);
        }
        printf("\n");
    }
    if (ret < 0) {
        ret = 1000;
    }
	free(mat1);
	free(mat2);
    // It's row #1 because we swap rows at the end of each outer loop,
    // as we are to return the last number on the lowest row
    return ret;
}
// Callback for the test protocol
static int callback_test(struct lws* wsi, enum lws_callback_reasons reason, void *user, void* in, size_t len)
{
	printf("---------------Lock2\n");
	pthread_mutex_lock(&ws_mutex);
	pj_status_t status;
	pj_thread_desc aPJThreadDesc;
	if (!pj_thread_is_registered()) {
		pj_thread_t *pjThread;
		status = pj_thread_register(NULL, aPJThreadDesc, &pjThread);
		if (status != PJ_SUCCESS) {
		}
	}
	
	struct call_info *this_call_info;
	int call_index;

	call_index = find_index_from_websocket(wsi);
	if (call_index == -1) {
		this_call_info = NULL;	
		// printf("<<**>> callback_test  (threadid: NULL, call_id: NULL, %X, %d)\n", wsi, reason);
	} else {
		this_call_info = current_calls[call_index];	

		// printf("<<**>> callback_test  (threadid: %d, call_id: %d)\n", this_call_info->ws_thread_id, this_call_info->call_id);
	}
	int call_id = this_call_info != NULL ? this_call_info->call_id : -1;

	// The message we send back to the echo server
	const char msg[128] = "{\"action\": \"start\", \"content-type\": \"audio/l16;rate=16000\", \"interim_results\": true}";
	const char stopmsg[128] = "{\"action\": \"stop\"}";
	// The buffer holding the data to send
	// NOTICE: data which is sent always needs to have a certain amount of memory (LWS_PRE) preserved for headers
	unsigned char buf[LWS_PRE + 128];
	unsigned char stopbuf[LWS_PRE + 128];
	
	// Allocating the memory for the buffer, and copying the message to it
	memset(&buf[LWS_PRE], 0, 128);
	memset(&stopbuf[LWS_PRE], 0, 128);
	
	strncpy((char*)buf + LWS_PRE, msg, 128);
	strncpy((char*)stopbuf + LWS_PRE, stopmsg, 128);

	// For which reason was this callback called?
	switch (reason)
	{
	case LWS_CALLBACK_CLOSED:
	case LWS_CALLBACK_WSI_DESTROY:
		PJ_LOG(1, (THIS_FILE, "[Test Protocol %d] Connection closed.\n", call_id));
		if (call_index != -1) {
			pjsua_call_info ci;
			pjsua_call_get_info(this_call_info->call_id, &ci);
			call_hangup_retry(this_call_info->call_id, &ci);
		}

		break;

		// Our client received something

	case LWS_CALLBACK_CLIENT_RECEIVE:
		// printf("callback_test LWS_CALLBACK_CLIENT_RECEIVE.\n");
		{
			if (call_index != -1) {
				
				// Parse JSON
				json_char* json;
				json_value* value;
				int json_size = strlen((char*)in);

				json = (json_char*)in;

				value = json_parse(json,json_size);

				if (value == NULL) {
					PJ_LOG(1, (THIS_FILE, "[Test Protocol %d] Unable to parse data\n", call_id));
					break;
				}
				
				// process_value(value, 0);
				if (value->type != json_object) {
					// printf("----------- results not fetch");
					break;
				}
				json_value *results = value->u.object.values[0].value;
				if (results->type != json_array) {
					// printf("----------- first result not fetch");
					break;
				}
				json_value *first_result = results->u.array.values[0];
				if (first_result->type != json_object) {
					// prieeentf("----------- is final object not fetch");
					break;
				}
				json_value *first_result_final = first_result->u.object.values[1].value;
				if (first_result_final->type != json_boolean) {
					// printf("----------- is final is not boolean");
					break;
				}
				json_value *alternatives = first_result->u.object.values[0].value;
				if (alternatives->type != json_array) {
					break;
				}
				json_value *firstAlt = alternatives->u.array.values[0];
				if (firstAlt->type != json_object) {
					break;
				}
				json_value *trans = NULL;
				for (int k = 0; k < firstAlt->u.object.length; k ++) {
					if (strcmp(firstAlt->u.object.values[k].name, "transcript") == 0) {
						trans = firstAlt->u.object.values[k].value;
						break;
					}
				}
				if (trans) {
					char *transcription = trans->u.string.ptr;
					// printf("is_final %s", is_final ? "true" : "false");
					int is_final = first_result_final->u.boolean;
					if (is_final) {
						if (call_index != -1) {
							
							// printf("<<**>> callback_test  (this_call_info: 0x%x) \n", this_call_info);
							PJ_LOG(1, (THIS_FILE, "<<**>> callback_test  (threadid: %d, call_id: %d):%s\n", this_call_info->ws_thread_id, call_id, transcription));
							
							// ignore response while sending dtmf
							if (this_call_info->sending) {
								return;
							}

							if (this_call_info->isProfileI && this_call_info->transcription[0] != '\0') {
								int ci = user_input_cnt; // current response index
								strcpy(user_input_list[ci], this_call_info->transcription);
								strcpy(response_list[ci], "Skip\n");
								this_call_info->transcription[0] = '\0';
								user_input_cnt ++;
							}
							strcpy(this_call_info->transcription, transcription);

							if(this_call_info->pi) {
								pipe_producer_t* p = pipe_producer_new(this_call_info->transcriptions);
								pipe_push(p, transcription, 1);
								pipe_producer_free(p);
								pthread_t send_thread_id;
								pthread_create(&send_thread_id, NULL, send_thread_func, this_call_info);
							}
						}
						// if (strstr(transcription, "other options") != NULL) {
						// 	// Check if include "other options" and send wav by player
						// 	vector_push_back(send_queue, "other_options.wav");
						// }
						//bExit = 1;
						// return -1; // Returning -1 causes the client to disconnect from the server
					}
				}
				json_value_free(value);
			}
		}
			break;

	// The connection was successfully established
	case LWS_CALLBACK_CLIENT_ESTABLISHED:
		PJ_LOG(1, (THIS_FILE, "[Test Protocol %d] Connection to server established.\n", call_id));

		PJ_LOG(1, (THIS_FILE, "[Test Protocol %d] Writing \"%s\" to server.\n", call_id, msg));

		lws_write(wsi, &buf[LWS_PRE], strlen(msg), LWS_WRITE_TEXT);

		// Write the buffer from the LWS_PRE index + 128 (the buffer size)
		lws_callback_on_writable(wsi);
		break;

		// The server notifies us that we can write data
	case LWS_CALLBACK_CLIENT_WRITEABLE:
		// PJ_LOG(1, (THIS_FILE, "[Test Protocol %d] The client is able to write.\n", call_id));
		if (call_index != -1) {
			if (this_call_info->bufferSize == 0) {
				lws_callback_on_writable(wsi);
				break;
			}
			if (this_call_info->shouldSendStop) {
				lws_write(wsi, &stopbuf[LWS_PRE], strlen(stopmsg), LWS_WRITE_TEXT);
				PJ_LOG(1, (THIS_FILE, "[Test Protocol %d] Issue Stop\n", call_id));
				this_call_info->shouldSendStop = 0;
				lws_callback_on_writable(wsi);
				break;
			}
			// printf("LWS_CALLBACK_CLIENT_WRITEABLE1\n");
			pthread_mutex_lock(&this_call_info->ws_buf_mutex);
			// printf("LWS_CALLBACK_CLIENT_WRITEABLE2\n");
			// char *binary_buf = malloc(sizeof(char) * (LWS_PRE + this_call_info->bufferSize));
			memcpy(&binary_buf[LWS_PRE], this_call_info->globalBuf, this_call_info->bufferSize);
			lws_write(wsi, &binary_buf[LWS_PRE], this_call_info->bufferSize, LWS_WRITE_BINARY);
			// free(binary_buf);
			// printf("LWS_CALLBACK_CLIENT_WRITEABLE3\n");
			this_call_info->bufferSize = 0;
			// printf("LWS_CALLBACK_CLIENT_WRITEABLE4\n");
			// printf("Freeing %x...", this_call_info->globalBuf);
			// printf("LWS_CALLBACK_CLIENT_WRITEABLE5\n");
			pthread_mutex_unlock(&this_call_info->ws_buf_mutex);
			lws_callback_on_writable(wsi);
		}
		break;

		// There was an error connecting to the server
	case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
		PJ_LOG(1, (THIS_FILE, "[Test Protocol %d] There was a connection error: %s\n", call_id, in ? (char*)in : "(no error information)"));
		if (this_call_info != NULL) {
			pjsua_call_info ci;
			pjsua_call_get_info(this_call_info->call_id, &ci);
			call_hangup_retry(this_call_info->call_id, &ci);
		}
		break;
	default:
		// if (reason != 34 && reason != 35 && reason != 36 && reason != 71) {
		// 	PJ_LOG(1, (THIS_FILE, "[Test Protocol %d] Unhandled reason : %d\n", call_id, reason));
		// }
		
		break;
	}
	pthread_mutex_unlock(&ws_mutex);
	printf("-------------UnLock4\n");
	// printf("<<**>> callback_test ended");
	return 0;
}

static PJ_DEF(pj_status_t) on_pjsua_wav_file_end_callback(pjmedia_port* media_port, void* args)
{
    pj_status_t status;

    struct pjsua_player_eof_data *eof_data = (struct pjsua_player_eof_data *)args;

	pj_pool_release(eof_data->pool);

    status = pjsua_player_destroy(eof_data->player_id);

    PJ_LOG(1,(THIS_FILE, "End of Wav File, media_port: %d", media_port));

    if (status == PJ_SUCCESS)
    {
        return -1;// Here it is important to return a value other than PJ_SUCCESS
                  //Check link below
    }

    return PJ_SUCCESS;
}

void *send_thread_func(void *vargp) {
	struct call_info *this_call_info = (struct call_info *)vargp;

	pj_status_t status;
	pj_thread_desc aPJThreadDesc;
	if (!pj_thread_is_registered()) {
		pj_thread_t *pjThread;
		status = pj_thread_register(NULL, aPJThreadDesc, &pjThread);
		if (status != PJ_SUCCESS) {
		}
	}

	if (this_call_info->transcriptions->begin + this_call_info->transcriptions->elem_size == this_call_info->transcriptions->end)
		return NULL;
	char transcription[1000];
	pipe_consumer_t* c = pipe_consumer_new(this_call_info->transcriptions);
	size_t ret = pipe_pop(c, transcription, 1);
	pipe_consumer_free(c);
	if (ret > 0) {
		int found_action = 0;
		int smallest_difference = 100000;
		char similarest[1000], tempa[1000];
		struct profile_info *pi = this_call_info->pi;
		int k;
		for (k = this_call_info->prv_ran_cmd_id + 1; k < pi->number_commands; k ++) {
			if (strcmp(pi->cmd[k][0], "EXT") == 0) continue;
			int limit = strlen(pi->user_input_list[k])/7 + 7;
			strcpy(tempa, transcription);
			// printf("<<**>> difference between \"%s\" and \"%s\"\n", pi->user_input_list[k], transcription);
			// if (strlen(pi->user_input_list[k]) < strlen(transcription)) {
			// 	strcpy(tempa, transcription + strlen(transcription) - strlen(pi->user_input_list[k]));
			// 	printf("TRimmed: %s\n", tempa);
			// }
			int difference = getDifference(pi->user_input_list[k], tempa);
			// printf("<<**>> difference %d %d\n", difference, limit);
			
			if(difference < limit || strstr(transcription, pi->user_input_list[k]) != NULL) {
				// printf("going to show 0x%x <<------>> 0x%x <<>--------> 0x%x\n", this_call_info, pi, transcription );
				// printf("%d) found similar setences, \"%s\" and \"%s\"\n", this_call_info->ci, pi->user_input_list[k], transcription);

				PJ_LOG(1, (THIS_FILE, "going to do \"%s\" command \n", pi->cmd[k][0]));
				if (strcmp(pi->cmd[k][0], "Skip") == 0){
				} else if (strcmp(pi->cmd[k][0], "Dial") == 0) {
					if (strcmp(pi->cmd[k][1], "-L") == 0) {// Dial -L -T /files/zipcodelist.txt
						FILE *fp = fopen (pi->cmd[k][3], "r");
						char new_line[100];
						char to_dial_number[100];
						int index = 0;
						if(fp == NULL) {
							PJ_LOG(1, (THIS_FILE, "Cannot read number list, it doesn't exist! --> filename = %s\n", pi->cmd[k][3]));
						} else {
							PJ_LOG(1, (THIS_FILE, "calculating size of number list\n"));
							while (1) {
								if (fgets(new_line,150, fp) == NULL) break;
								if(index == this_call_info->ci) {
									strcpy(to_dial_number, new_line);
									to_dial_number[strcspn(to_dial_number, "\n")] = 0;
								}
								index++;
							}
							fclose(fp);
						}
						on_dial_command(this_call_info, to_dial_number);
						this_call_info->prv_ran_cmd_id = k;
						strcpy(this_call_info->prv_ran_cmd_param, to_dial_number);
					} else { // Dial 12345
						on_dial_command(this_call_info, pi->cmd[k][1]);
						this_call_info->prv_ran_cmd_id = k;
						strcpy(this_call_info->prv_ran_cmd_param, pi->cmd[k][1]);
					}	
				} else if (strcmp(pi->cmd[k][0], "Speak") == 0) {//"billing"
					on_speak_command(pi->cmd[k][1], this_call_info->call_id);
					this_call_info->prv_ran_cmd_id = k;
					strcpy(this_call_info->prv_ran_cmd_param, pi->cmd[k][1]);
				} else if (strcmp(pi->cmd[k][0], "EXT") == 0) {//EXT /files/responses.txt
				}
				found_action = 1;
				break;
			} else {
				if (smallest_difference > difference) {
					smallest_difference = difference;
					strcpy(similarest, pi->user_input_list[k]);
				}
			}
		}

		if (found_action == 0) {
			int lci = this_call_info->prv_ran_cmd_id;
			char lcp[100];
			strcpy(lcp,  this_call_info->prv_ran_cmd_param);
			if (lci < pi->number_commands - 1 && strcmp(pi->cmd[lci+1][0], "EXT") == 0) {
				PJ_LOG(1, (THIS_FILE, "---------Going to extract----------- prv_ran_cmd_id: %d num_commands: %d \n", lci, pi->number_commands));
				PJ_LOG(1, (THIS_FILE, "<<**>> current transcription result save start\n %s \n", pi->cmd[lci][0]));
				pthread_mutex_lock(&write_ext_mutex);

				FILE *fp = fopen (pi->cmd[lci+1][1], "a"); 
				PJ_LOG(1, (THIS_FILE, "<start ci=%d cmd=%s param=%s>--------------<start>\n", this_call_info->ci, pi->cmd[lci][0], lcp));
				fprintf(fp, "<start ci=%d cmd=%s param=%s call=%d>--------------<start>\n", this_call_info->ci, pi->cmd[lci][0], lcp, this_call_info->call_id);
				fprintf(fp, "%s\n", transcription);
				fprintf(fp, "<end>--------------<end>\n");
				fclose(fp);
				this_call_info->done_ext = 1;
				pthread_mutex_unlock(&write_ext_mutex);
			}

			// PJ_LOG(1, (THIS_FILE, "<<***>> call hanging up -start since not recognized:\n\"%s\"\n", transcription));
			// PJ_LOG(1, (THIS_FILE, "<<<<>>>> similarest setence is %s, difference =%d\n", similarest, smallest_difference));

			// pjsua_call_info ci;
			// pjsua_call_get_info(this_call_info->call_id, &ci);
			// // pjsua_conf_disconnect(ci.conf_slot, this_call_info->rec_slot);
			// // this_call_info->rec_slot = PJSUA_INVALID_ID;
			// pjsua_call_hangup(this_call_info->call_id, 0, NULL, NULL);
			PJ_LOG(1, (THIS_FILE, "<<***>> unrecognized below response:\n\"%s\"\n", transcription));
		}
	}
	
	return NULL;
}

struct MemoryStruct {
  	char *memory;
  	size_t size;
};

static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
  size_t realsize = size * nmemb;
  struct MemoryStruct *mem = (struct MemoryStruct *)userp;
 
  char *ptr = realloc(mem->memory, mem->size + realsize + 1);
  if(ptr == NULL) {
    /* out of memory! */ 
    return 0;
  }
 
  mem->memory = ptr;
  memcpy(&(mem->memory[mem->size]), contents, realsize);
  mem->size += realsize;
  mem->memory[mem->size] = 0;
 
  return realsize;
}

void * create_websocket(void *vargp) {
	printf("------------Lock1\n");
	pthread_mutex_lock(&ws_mutex);
	struct call_info *this_call_info = (struct call_info *)vargp;

	// signal(SIGINT, onSigInt);
	// Connection info
	CURL *hnd = curl_easy_init();

	struct MemoryStruct chunk;
 
	chunk.memory = malloc(1);  /* will be grown as needed by the realloc above */ 
	chunk.size = 0;    /* no data at this point */ 

	curl_easy_setopt(hnd, CURLOPT_CUSTOMREQUEST, "GET");
	curl_easy_setopt(hnd, CURLOPT_URL, "https://stream.watsonplatform.net/authorization/api/v1/token?url=https://stream.watsonplatform.net/speech-to-text/api");

	/* allow whatever auth the server speaks */
	curl_easy_setopt(hnd, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
	curl_easy_setopt(hnd, CURLOPT_USERPWD, "16079a80-8095-4f27-9261-ff6f9031fe9d:tChEs4oZVHNd");

	/* send all data to this function  */ 
	curl_easy_setopt(hnd, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
	
	/* we pass our 'chunk' struct to the callback function */ 
	curl_easy_setopt(hnd, CURLOPT_WRITEDATA, (void *)&chunk);
	CURLcode res = curl_easy_perform(hnd);

	if(res != CURLE_OK) {
		printf("------------UnLock1\n");
		pthread_mutex_unlock(&ws_mutex);
      	fprintf(stderr, "curl_easy_perform() failed: %s\n",
              curl_easy_strerror(res));
		return NULL;
	}

	curl_easy_cleanup(hnd);
 

	char inputURL[5000];
	sprintf(inputURL, "wss://stream.watsonplatform.net/speech-to-text/api/v1/recognize?watson-token=%s", chunk.memory);
	// printf(inputURL);
	free(chunk.memory);
	// strcpy(inputURL, "wss://echo.websocket.org");

	struct lws_context_creation_info ctxCreationInfo; // Context creation info
	struct lws_client_connect_info clientConnectInfo; // Client creation info
	struct lws_context *ctx; // The context to use

	const char *urlProtocol, *urlTempPath; // the protocol of the URL, and a temporary pointer to the path
	char urlPath[5000]; // The final path string

	// Set both information to empty and allocate it's memory
	memset(&ctxCreationInfo, 0, sizeof(ctxCreationInfo));
	memset(&clientConnectInfo, 0, sizeof(clientConnectInfo));
	
	// Parse the input url (e.g. wss://echo.websocket.org:1234/test)
	//   the protocol (wss)
	//   the address (echo.websocket.org)
	//   the port (1234)
	//   the path (/test)
	if (lws_parse_uri(inputURL, &urlProtocol, &clientConnectInfo.address, &clientConnectInfo.port, &urlTempPath))
	{
		printf("Couldn't parse URL\n");
	}

	// Fix up the urlPath by adding a / at the beginning, copy the temp path, and add a \0 at the end
	urlPath[0] = '/';
	strncpy(urlPath + 1, urlTempPath, sizeof(urlPath) - 2);
	urlPath[sizeof(urlPath) - 1] = '\0';
//	urlPath[0] = '\0';
	clientConnectInfo.path = urlPath; // Set the info's path to the fixed up url path

	// Set up the context creation info
	ctxCreationInfo.port = CONTEXT_PORT_NO_LISTEN; // We don't want this client to listen
	ctxCreationInfo.protocols = protocols; // Use our protocol list

	ctxCreationInfo.gid = -1; // Set the gid and uid to -1, isn't used much
	ctxCreationInfo.uid = -1;
	// ctxCreationInfo.extensions = extensions; // Use our extensions list
	// Create the context with the info
	printf("%d\n", ctxCreationInfo.options);
	ctxCreationInfo.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT; // | 
							//   LWS_SERVER_OPTION_ALLOW_NON_SSL_ON_SSL_PORT | 
							//   LWS_SERVER_OPTION_PEER_CERT_NOT_REQUIRED | 
							//   LWS_SERVER_OPTION_ALLOW_LISTEN_SHARE;
	ctx = lws_create_context(&ctxCreationInfo);
	
	if (ctx == NULL)
	{
		printf("-----------UnLock2\n");
		pthread_mutex_unlock(&ws_mutex);
		printf("Error creating context\n");
		return NULL;
	}
	printf("-----------UnLock3\n");
	pthread_mutex_unlock(&ws_mutex);
	// LCCSCF_USE_SSL 				= (1 << 0),
	// LCCSCF_ALLOW_SELFSIGNED			= (1 << 1),
	// LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK	= (1 << 2),
	// LCCSCF_ALLOW_EXPIRED			= (1 << 3),

	// LCCSCF_PIPELINE				= (1 << 16),
	// Set up the client creation info

	clientConnectInfo.context = ctx; // Use our created context
	clientConnectInfo.ssl_connection = LCCSCF_USE_SSL | LCCSCF_ALLOW_SELFSIGNED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK | LCCSCF_ALLOW_EXPIRED; // Don't use SSL for this test
	clientConnectInfo.host = clientConnectInfo.address; // Set the connections host to the address
	clientConnectInfo.origin = clientConnectInfo.address; // Set the conntections origin to the address
	clientConnectInfo.ietf_version_or_minus_one = -1; // IETF version is -1 (the latest one)
	static int index = 1;
	clientConnectInfo.protocol = protocols[index].name; // We use our test protocol
	index %= 5;
	index += 1;
	clientConnectInfo.pwsi = &this_call_info->wsiTest; // The created client should be fed inside the wsi_test variable

	// printf("Connecting to %s://%s:%d%s \n\n", urlProtocol, clientConnectInfo.address, clientConnectInfo.port, urlPath);

	// Connect with the client info
	lws_client_connect_via_info(&clientConnectInfo);
	
	printf("<<**>> updated wsiTest on create_websocket %X\n", this_call_info->wsiTest);
	if (this_call_info->wsiTest == NULL)
	{
		printf("Error creating the client\n");
		return NULL;
	}

	// Main loop runs till bExit is true, which forces an exit of this loop
	// TODO: need to exit from websocket callback when disconnected
	while (!bExit)
	{
		if (this_call_info -> disconnected == 1) {
			if (this_call_info->wsiTest == NULL) {
				printf("<<**>> do free of this_call_info=%x\n", this_call_info);
				free(this_call_info);
			}
			break;
		}
		// LWS' function to run the message loop, which polls in this example every 50 milliseconds on our created context
		// printf("%x ", ctx);
		lws_service(ctx, 50);
	}

	// Destroy the context
	lws_context_destroy(ctx);

	printf("\nDone executing.\n");

	printf("<<**>> create_websocket ended");
	return NULL;
}

// Write to file being downloaded
size_t write_to_file(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    size_t written = fwrite(ptr, size, nmemb, stream);
    return written;
}

int download_wav(char *sentence)
{
	printf("---->downloading %s<----", sentence);
	CURL *hnd = curl_easy_init();

	struct MemoryStruct chunk;
 
	chunk.memory = malloc(1);  /* will be grown as needed by the realloc above */ 
	chunk.size = 0;    /* no data at this point */ 

	curl_easy_setopt(hnd, CURLOPT_CUSTOMREQUEST, "POST");
	curl_easy_setopt(hnd, CURLOPT_URL, "https://westus.api.cognitive.microsoft.com/sts/v1.0/issueToken");

	struct curl_slist *headers = NULL;
	headers = curl_slist_append(headers, "Ocp-Apim-Subscription-Key: 47e8b1ae22234b6ebd0674262c1afa40");
	headers = curl_slist_append(headers, "Content-Length: 0");
	curl_easy_setopt(hnd, CURLOPT_HTTPHEADER, headers);

	/* send all data to this function  */ 
	curl_easy_setopt(hnd, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
	
	/* we pass our 'chunk' struct to the callback function */ 
	curl_easy_setopt(hnd, CURLOPT_WRITEDATA, (void *)&chunk);
	CURLcode res = curl_easy_perform(hnd);

	if(res != CURLE_OK) {
      	fprintf(stderr, "curl_easy_perform() failed: %s\n",
              curl_easy_strerror(res));
		return -1;
	}

	curl_easy_cleanup(hnd);

	char authHeader[1000];
	sprintf(authHeader, "Authorization: %s", chunk.memory);
 
	free(chunk.memory);

	// Another curl for download wav
	hnd = curl_easy_init();

	char outfilename[FILENAME_MAX];
	sprintf(outfilename, "%s.wav", sentence);
	FILE *fp = fopen(outfilename,"wb");
 
	curl_easy_setopt(hnd, CURLOPT_CUSTOMREQUEST, "POST");
	curl_easy_setopt(hnd, CURLOPT_URL, "https://westus.tts.speech.microsoft.com/cognitiveservices/v1");

	headers = NULL;
	headers = curl_slist_append(headers, authHeader);
	headers = curl_slist_append(headers, "X-Microsoft-OutputFormat: riff-16khz-16bit-mono-pcm");
	headers = curl_slist_append(headers, "Content-Type: application/ssml+xml");
	headers = curl_slist_append(headers, "User-Agent: XXX");
	curl_easy_setopt(hnd, CURLOPT_HTTPHEADER, headers);

	char postData[2000];
	sprintf(postData, "<speak version='1.0' xmlns=\"http://www.w3.org/2001/10/synthesis\" xml:lang='en-US'>\n<voice  name='Microsoft Server Speech Text to Speech Voice (en-US, JessaRUS)'>\n    %s\n</voice> </speak>", sentence);
	curl_easy_setopt(hnd, CURLOPT_POSTFIELDS, postData);

	/* send all data to this function  */ 
	curl_easy_setopt(hnd, CURLOPT_WRITEFUNCTION, write_to_file);
	
	/* we pass our 'chunk' struct to the callback function */ 
	curl_easy_setopt(hnd, CURLOPT_WRITEDATA, fp);
	res = curl_easy_perform(hnd);

	if(res != CURLE_OK) {
      	fprintf(stderr, "curl_easy_perform() failed: %s\n",
              curl_easy_strerror(res));
		return -1;
	}
	fclose(fp);
	curl_easy_cleanup(hnd);

	printf("<<**>> downloadwav ended");
	return 0;
}

////////////////////////////////////////////////////////////////
///             Delimit by spaces unless it's in quotes

size_t handle_quoted_argument(char *str, char **destination) {
    assert(*str == '\"');
    /* discard the opening quote */
    *destination = str + 1;
 
    /* find the closing quote (or a '\0' indicating the end of the string) */
    size_t length = strcspn(str + 1, "\"") + 1;
    assert(str[length] == '\"'); /* NOTE: You really should handle mismatching quotes properly, here */
 
    /* discard the closing quote */
    str[length] = '\0';
    return length + 1;
}
 
size_t handle_unquoted_argument(char *str, char **destination) {
    size_t length = strcspn(str, " \n");
    char c = str[length];
    *destination = str;
    str[length] = '\0';
    return c == ' ' ? length + 1 : length;
}
 
size_t handle_whitespace(char *str) {
    int whitespace_count;
    /* This will count consecutive whitespace characters, eg. tabs, newlines, spaces... */
    assert(sscanf(str, " %n", &whitespace_count) == 0);
    return whitespace_count;
}

enum profile_mode {
	PROFILE_NONE,
	PROFILE_INPUT,
	PROFILE_RUN
};
enum profile_mode level = PROFILE_NONE;

////////////////////////
// DTMF
struct call_dtmf_data *call_init_tonegen(pjsua_call_id call_id)
{
  pj_pool_t *pool;
  struct call_dtmf_data *cd;
  pjsua_call_info ci;

  pool = pjsua_pool_create("call%p", 500, 500);
  
  cd = PJ_POOL_ZALLOC_T(pool, struct call_dtmf_data);
  cd->pool = pool;

  pjmedia_tonegen_create(cd->pool, 8000, 1, 160, 16, 0, &cd->tonegen);
  pjsua_conf_add_port(cd->pool, cd->tonegen, &cd->toneslot);

  pjsua_call_get_info(call_id, &ci);
  pjsua_conf_connect(cd->toneslot, ci.conf_slot);

  pjsua_call_set_user_data(call_id, (void*) cd);

  return cd;
}

void call_play_digit(pjsua_call_id call_id, const char *digits)
{
  pjmedia_tone_digit d[16];
  unsigned i, count = strlen(digits);
  struct call_dtmf_data *cd;
  PJ_LOG(1, (THIS_FILE, "count: %d\n", count));

  cd = (struct call_dtmf_data*) pjsua_call_get_user_data(call_id);
  if (!cd)
     cd = call_init_tonegen(call_id);

  if (count > PJ_ARRAY_SIZE(d))
    count = PJ_ARRAY_SIZE(d);

  pj_bzero(d, sizeof(d));
  for (i=0; i<count; ++i) {
    d[i].digit = digits[i];
    d[i].on_msec = 300;
    d[i].off_msec = 500;
    d[i].volume = 0;
  }
	PJ_LOG(1, (THIS_FILE, "--------sending dtmf\n-"));
  pjmedia_tonegen_play_digits(cd->tonegen, count, d, 0);
}

void call_deinit_tonegen(pjsua_call_id call_id)
{
  struct call_dtmf_data *cd;

  cd = (struct call_dtmf_data*) pjsua_call_get_user_data(call_id);
  if (!cd)
     return;

  pjsua_conf_remove_port(cd->toneslot);
  pjmedia_port_destroy(cd->tonegen);
  pj_pool_release(cd->pool);

  pjsua_call_set_user_data(call_id, NULL);
  PJ_LOG(1, (THIS_FILE, "DEINIT TONE GEN --- %d\n", call_id));
}

void store_response(char *response) {
	PJ_LOG(1, (THIS_FILE, "<<**>> store_response started"));
	struct call_info *this_call_info;
	int call_index;
	call_index = find_index_profile_insert();
	if (call_index != -1) {
		this_call_info = current_calls[call_index];
	}

	if (call_index == -1) {
		PJ_LOG(1, (THIS_FILE, "call_index == 0 and returning\n"));
		return;
	}
	PJ_LOG(1, (THIS_FILE, "<<**>>  store_response -> %d\n", this_call_info->ws_thread_id));
	PJ_LOG(1, (THIS_FILE, "store_response -> %s\n", response));
	PJ_LOG(1, (THIS_FILE, "store_response -> %s\n", this_call_info->transcription));
	int ci = user_input_cnt; // current response index
	strcpy(user_input_list[ci], this_call_info->transcription);
	strcpy(response_list[ci], response);
	this_call_info->transcription[0] = '\0';
	user_input_cnt ++;
	PJ_LOG(1, (THIS_FILE, "<<**>> store_response ended"));
}
void save_user_responses() {
	PJ_LOG(1, (THIS_FILE, "<<**>> save_user_responses started"));
	if (!current_profile_name) {
		PJ_LOG(1, (THIS_FILE, "currently profile name is null\n"));
		return;
	}
	int i = 0;
	FILE *fp = fopen (current_profile_name, "a");
	for (; i < user_input_cnt; i ++) {
		fprintf(fp, "%s\n", user_input_list[i]);
		fprintf(fp, "%s\n", response_list[i]);
		PJ_LOG(1, (THIS_FILE, "userinput -> %s\n", user_input_list[i]));
		PJ_LOG(1, (THIS_FILE, "response -> %s\n", response_list[i]));
		user_input_list[i][0] = '\0';
		response_list[i][0] = '\0';
	}
	user_input_cnt = 0;
	current_profile_name = NULL;
	fclose(fp);
	PJ_LOG(1, (THIS_FILE, "<<**>> save_user_responses ended"));
}

void *process_call(void *vargp) {
	pj_status_t status;
	pj_thread_desc aPJThreadDesc;
	if (!pj_thread_is_registered()) {
		pj_thread_t *pjThread;
		status = pj_thread_register(NULL, aPJThreadDesc, &pjThread);
		if (status != PJ_SUCCESS) {
		}
	}
	
	while (1) {
		int size = vector_size(current_profile_info.call_queue);
		PJ_LOG(1, (THIS_FILE, "Remaining: %d", vector_size(current_profile_info.call_queue)));
		if (size > 0 && vector_size(current_calls) < CCCC) {
			// PJ_LOG(1, (THIS_FILE, "<<**>> make_call_to_profile thread started"));
			struct call_to_profile_with_number* call = current_profile_info.call_queue[size - 1];
			vector_pop_back(current_profile_info.call_queue);

			struct profile_info *pi = call->pi;
			int number = call->number;
			int tried_cnt= call->tried_cnt;
			
			char contact[200];
			sprintf(contact, "sip:%s@%s", pi->phone, SIP_DOMAIN);
			// PJ_LOG(1, (THIS_FILE, "<<**>> contact=%s\n", contact));
			
			struct call_info *newCall = malloc( sizeof(struct call_info) );
			init_call_info(newCall);

			strcpy(newCall->callerId, call->callerId);

			vector_push_back(current_calls, newCall);

			struct call_info *this_call_info = current_calls[vector_size(current_calls)-1];


			this_call_info->pi = pi;
			this_call_info->ci = number;
			this_call_info->tried_cnt = tried_cnt;
			// Create recognition thread
			pthread_create(&this_call_info->ws_thread_id, NULL, create_websocket,(void *) (this_call_info));

			pj_str_t uri = pj_str(contact);

			// Custom header
			pjsua_msg_data msg_data_;
			pjsip_generic_string_hdr warn;
			pj_str_t hname = pj_str("Custom");
			pj_str_t hvalue = pj_str(call->callerId);
			pjsua_msg_data_init(&msg_data_);
			pjsip_generic_string_hdr_init2(&warn, &hname, &hvalue);
			pj_list_push_back(&msg_data_.hdr_list, &warn);

			status = pjsua_call_make_call(*shared_acc_id, &uri, 0, NULL, &msg_data_, &this_call_info->call_id);
			if (status != PJ_SUCCESS)
				error_exit("Error making call", status);
			
			free(call);
		}
		sleep(2);
	}
	
	return NULL;
}

void *make_call_to_profile(void *vargp) {
	sleep(2);
	pj_status_t status;
	pj_thread_desc aPJThreadDesc;
	if (!pj_thread_is_registered()) {
		pj_thread_t *pjThread;
		status = pj_thread_register(NULL, aPJThreadDesc, &pjThread);
		if (status != PJ_SUCCESS) {
		}
	}
	
	PJ_LOG(1, (THIS_FILE, "<<**>> make_call_to_profile thread started"));
	struct call_to_profile_with_number thread_param = *(struct call_to_profile_with_number *) vargp;
	free(vargp);
	
	struct profile_info *pi = thread_param.pi;
	int number = thread_param.number;
	int tried_cnt= thread_param.tried_cnt;
	
	char contact[200];
	sprintf(contact, "sip:%s@%s", pi->phone, SIP_DOMAIN);
	PJ_LOG(1, (THIS_FILE, "<<**>> contact=%s\n", contact));
	
	struct call_info *newCall = malloc( sizeof(struct call_info) );
	init_call_info(newCall);

	strcpy(newCall->callerId, thread_param.callerId);

	vector_push_back(current_calls, newCall);
	struct call_info *this_call_info = current_calls[vector_size(current_calls)-1];

	this_call_info->pi = pi;
	this_call_info->ci = number;
	this_call_info->tried_cnt = tried_cnt;
	// Create recognition thread
	pthread_create(&this_call_info->ws_thread_id, NULL, create_websocket,(void *) (this_call_info));

	pj_str_t uri = pj_str(contact);

    // Custom header
    pjsua_msg_data msg_data_;
    pjsip_generic_string_hdr warn;
    pj_str_t hname = pj_str("Custom");
    pj_str_t hvalue = pj_str(thread_param.callerId);
    pjsua_msg_data_init(&msg_data_);
    pjsip_generic_string_hdr_init2(&warn, &hname, &hvalue);
    pj_list_push_back(&msg_data_.hdr_list, &warn);

	status = pjsua_call_make_call(*shared_acc_id, &uri, 0, NULL, &msg_data_, &this_call_info->call_id);
	if (status != PJ_SUCCESS)
		error_exit("Error making call", status);
	
	//TODO: Array of struct is not appropriate - need to use array of struct pointer
	
	return NULL;
}

void delimit_by_spaces(char *Line, pjsua_acc_id *acc_id) {
    char line[strlen(Line) + 1];
    char *args[sizeof line];
    size_t n = 0, argv = 0;
 
    strcpy(line, Line);
 
    while (line[n] != '\0') {
        n += handle_whitespace(line + n);
        n += line[n] == '\"' ? handle_quoted_argument(line + n, args + argv++)
                             : handle_unquoted_argument(line + n, args + argv++);
    }

	// check last argument empty
	if (strlen(args[argv - 1]) == 0) {
		argv--;
	}

	if (strcmp(args[0], "Profile") == 0) {
		if (strcmp(args[1], "-C") == 0) {
			if (argv != 5) {
				puts("Wrong format");
				return;
			}
			FILE *fp = fopen (args[2], "w+");
   			fprintf(fp, "%s %s\n", args[3], args[4]);
			fclose(fp);
			puts("Profile created");
		} else if (strcmp(args[1], "-I") == 0) {
			if (argv != 3) {
				puts("Wrong format");
				return;
			}
			current_profile_name = args[2];
			FILE *fp = fopen (args[2], "r");
			if(fp == NULL) {
				puts("Profile doesn't exist");
				return;
			}
			char line[200], phone[200], phonenumbers[200];
			fgets (line, 60, fp);
			if (line == NULL) {
				puts("Wrong profile format");
				return;
			}

			// Remove newline at ending
			line[strcspn(line, "\n")] = 0;
			fclose(fp);

			char *spacel = strpbrk(line, " ");
			if (spacel != NULL) {
				strcpy(phonenumbers, spacel + 1);
				*spacel = '\0';
				strcpy(phone, line);
			} else {
				fclose(fp);
				PJ_LOG(1, (THIS_FILE, "Wrong profile format\n"));
				return;
			}
			
			struct call_info *newCall = malloc( sizeof(struct call_info) );
			init_call_info(newCall);

			vector_push_back(current_calls, newCall);
			struct call_info *this_call_info = current_calls[vector_size(current_calls)-1];

			this_call_info->isProfileI = 1;
			// Create recognition thread
			pthread_create(&this_call_info->ws_thread_id, NULL, create_websocket,(void *) (this_call_info));

			char contact[200];
			sprintf(contact, "sip:%s@%s", phone, SIP_DOMAIN);
			puts(contact);
			pj_str_t uri = pj_str(contact);	

			// Custom header
			pjsua_msg_data msg_data_;
			pjsip_generic_string_hdr warn;
			pj_str_t hname = pj_str("Custom");
			pj_str_t hvalue = pj_str("19127537082");
			pjsua_msg_data_init(&msg_data_);
			pjsip_generic_string_hdr_init2(&warn, &hname, &hvalue);
			pj_list_push_back(&msg_data_.hdr_list, &warn);

			pj_status_t status = pjsua_call_make_call(*acc_id, &uri, 0, NULL, &msg_data_, &this_call_info->call_id);
			if (status != PJ_SUCCESS) 
				error_exit("Error making call", status);
			
			level = PROFILE_INPUT;
		} else if (strcmp(args[1], "-S") == 0) { // Profile -S "Wells Fargo"
			if (args[2] && strcmp(args[2], current_profile_name) == 0) {
				save_user_responses();
			} else {
				puts("filename does not match with currently editing profile");
			}
		}
	} else if (strcmp(args[0], "Run") == 0) {
		char listfilename[150]="";
		int cnt = 0;
		//read profile and analyze how many thread is needed
		FILE *fp = fopen (args[1], "r");
		if(fp == NULL) {
			puts("Cannot read profile!");
			return;
		}
		struct profile_info *pi = &current_profile_info;
		strcpy(pi->name, args[1]);
		pi->number_commands = 0;

		char new_line[300];
		int n_profile_lines = 0;
		while (1) {
			if (fgets(new_line,300, fp) == NULL) break;
			if (n_profile_lines == 0) {
				new_line[strcspn(new_line, "\n")] = 0;
				char *spacel = strpbrk(new_line, " ");
				if (spacel != NULL) {
					strcpy(pi->phonenumbers, spacel + 1);
					*spacel = '\0';
					strcpy(pi->phone, new_line);
				} else {
					fclose(fp);
					PJ_LOG(1, (THIS_FILE, "Wrong profile format\n"));
					return;
				}
			} else if (n_profile_lines % 3 == 1) {
				new_line[strcspn(new_line, "\n")] = 0;
				strcpy(pi->user_input_list[pi->number_commands], new_line);
			} else if (n_profile_lines %3 == 2) {
				char *new_args[sizeof new_line];
				size_t new_n = 0, new_argv = 0;
						
				while (new_line[new_n] != '\0') {
					new_n += handle_whitespace(new_line + new_n);
					new_n += new_line[new_n] == '\"' ? handle_quoted_argument(new_line + new_n, new_args + new_argv++)
										: handle_unquoted_argument(new_line + new_n, new_args + new_argv++);
				}

				// check last argument empty
				if (strlen(new_args[new_argv - 1]) == 0) {
					new_argv--;
				}

				pi->cmdLen[pi->number_commands] = new_argv;
				
				int k = 0;
				for (k = 0; k < new_argv; k ++ ){
					PJ_LOG(1, (THIS_FILE, "arg[%d]/%d , %s\n", k, new_argv, new_args[k]));
					strcpy(pi->cmd[pi->number_commands][k], new_args[k]);
				}
				if (new_argv == 4 && strcmp(new_args[1], "-L") == 0 && strcmp(new_args[2], "-T") == 0) {
					strcpy(listfilename, new_args[3]);
					PJ_LOG(1, (THIS_FILE, "got listfilename: %s\n", listfilename));
				}
				pi->number_commands ++;
			}
			n_profile_lines ++;
		}
		fclose(fp);

		//read profile and analyze how many thread is needed
		fp = fopen (listfilename, "r");

		//TODO find -D -T args and open a file read number of lines
		if(fp == NULL) {
			PJ_LOG(1, (THIS_FILE, "Cannot read number list, it doesn't exist! --> filename = %s\n", listfilename));
			cnt = 1;
		} else {
			PJ_LOG(1, (THIS_FILE, "calculating size of number list\n"));
			while (1) {
				if (fgets(new_line,150, fp) == NULL) break;
				cnt++;
			}
			fclose(fp);
			PJ_LOG(1, (THIS_FILE, "read number list --> filename = %s/cnt=%d\n", listfilename, cnt));
		}
		pi->thread_cnt = cnt;
		pi->finished_thread_cnt = 0;
		shared_acc_id = acc_id;

		// Read phone numbers
		fp = fopen (pi->phonenumbers, "r");
		if(fp == NULL) {
			fclose(fp);
			PJ_LOG(1, (THIS_FILE, "Phone numbers list doesn't exist\n"));
			return;
		}
		int j = 0;
		for (j = 0; j < cnt; j ++) {
			if (fgets(new_line, 20, fp) == NULL)
				strcpy(new_line, "18008008000");
			new_line[strcspn(new_line, "\n")] = 0;
			struct call_to_profile_with_number *thread_param = malloc(sizeof(struct call_to_profile_with_number));
			// pthread_t make_profile_call_thread_id;
			thread_param->pi = pi;
			thread_param->number = j;
			thread_param->tried_cnt = 0;
			strcpy(thread_param->callerId, new_line);

			vector_push_back(pi->call_queue, thread_param);
			// PJ_LOG(1, (THIS_FILE, ">>> just going to create thread for 'Run profile' %d/%d\n", j, cnt));
			// pthread_create(&make_profile_call_thread_id, NULL, make_call_to_profile, thread_param);
		}
		fclose(fp);
	} else if(current_profile_name) {

		struct call_info *this_call_info;
		int call_index;
		
		call_index = find_index_profile_insert();
		if (call_index != -1) {
			this_call_info = current_calls[call_index];
		}
		
		if (call_index == -1) {
			PJ_LOG(1, (THIS_FILE, "call_index == 0 and returning\n"));
			return;
		}

		if(this_call_info->transcription[0] == '\0'){
			return;
		}
		if (strcmp(args[0], "Skip") == 0){
			store_response(Line);
		} else if (strcmp(args[0], "Dial") == 0) {
			if (strcmp(args[1], "-L") == 0) {// Dial -L -T /files/zipcodelist.txt
				FILE *fp = fopen (args[3], "r");
				if(fp == NULL) {
					puts("Cannot read number list, it doesn't exist!");
					return;
				}
				char first_number[60];
				fgets (first_number, 60, fp);
				if (first_number == NULL) {
					puts("Cannot detect first number");
					return;
				}
				// Remove newline at ending
				first_number[strcspn(first_number, "\n")] = 0;
				fclose(fp);
				on_dial_command(this_call_info, first_number);
				store_response(Line);
			} else { // Dial 12345
				on_dial_command(this_call_info, args[1]);
				store_response(Line);
			}
		} else if (strcmp(args[0], "Speak") == 0) {//"billing"
			on_speak_command(args[1], this_call_info->call_id);
			store_response(Line);
		} else if (strcmp(args[0], "EXT") == 0) {//EXT /files/responses.txt
			store_response(Line);
		}
	}
}

/*
 * main()
 *
 * argv[1] may contain URL to call.
 */
int main(int argc, char *argv[])
{
	// lws_set_log_level(31, NULL); // We don't need to see the notice messages

	// download_wav("Other Options");
	// return 0;

	setvbuf (stdout, NULL, _IONBF, 0);
    pjsua_acc_id acc_id;
    pj_status_t status;

    /* Create pjsua first! */
    status = pjsua_create();
    if (status != PJ_SUCCESS) error_exit("Error in pjsua_create()", status);

    /* If argument is specified, it's got to be a valid SIP URL */
    if (argc > 1) {
	status = pjsua_verify_url(argv[1]);
	if (status != PJ_SUCCESS) error_exit("Invalid URL in argv", status);
    }

    /* Init pjsua */
    {
		pjsua_config cfg;
		pjsua_logging_config log_cfg;
		pjsua_media_config media_cfg;
		pjsua_config_default(&cfg);
		cfg.cb.on_call_media_state = &on_call_media_state;
		cfg.cb.on_call_state = &on_call_state;
		cfg.max_calls = 1000;
		cfg.thread_cnt = 16;

		pjsua_logging_config_default(&log_cfg);
		log_cfg.console_level = 4;

		pjsua_media_config_default(&media_cfg);
		media_cfg.thread_cnt = 16;

		status = pjsua_init(&cfg, &log_cfg, &media_cfg);
		if (status != PJ_SUCCESS) error_exit("Error in pjsua_init()", status);
    }

    /* Add UDP transport. */
    {
		pjsua_transport_config cfg;

		pjsua_transport_config_default(&cfg);
		cfg.port = 5060;
		status = pjsua_transport_create(PJSIP_TRANSPORT_UDP, &cfg, NULL);
		if (status != PJ_SUCCESS) error_exit("Error creating transport", status);
    }

    /* Initialization is done, now start pjsua */
    status = pjsua_start();
    if (status != PJ_SUCCESS) error_exit("Error starting pjsua", status);

	// status = pjsua_set_null_snd_dev();
	if (status != PJ_SUCCESS) {
	    return status;
    }

    /* Register to SIP server by creating SIP account. */
    {
		pjsua_acc_config cfg;

		pjsua_acc_config_default(&cfg);
		cfg.id = pj_str("sip:" SIP_USER "@" SIP_DOMAIN);
		cfg.reg_uri = pj_str("sip:" SIP_DOMAIN);
		cfg.cred_count = 1;
		cfg.cred_info[0].realm = pj_str(SIP_DOMAIN);
		cfg.cred_info[0].scheme = pj_str("digest");
		cfg.cred_info[0].username = pj_str(SIP_USER);
		cfg.cred_info[0].data_type = PJSIP_CRED_DATA_PLAIN_PASSWD;
		cfg.cred_info[0].data = pj_str(SIP_PASSWD);

		status = pjsua_acc_add(&cfg, PJ_TRUE, &acc_id);
		if (status != PJ_SUCCESS) error_exit("Error adding account", status);
    }

	memset(&current_profile_info, 0, sizeof(struct profile_info));

	pthread_t process_call_thread_id;
	pthread_create(&process_call_thread_id, NULL, process_call, NULL);

	char option[1000];
    /* Wait until user press "q" to quit. */
    for (;;) {

		puts("To create profile, run");
		puts("\t Profile -C \"profile_name\" phonenumber phonenumberslist.txt\n\n");

		puts("To provide instructions to the profile, run");
		puts("\t Profile -I \"profile_name\"\n\n");

		puts("After you finished instructions, run");
		puts("\t Profile -S \"profile_name\"\n\n");

		if (fgets(option, sizeof(option), stdin) == NULL) {
			puts("EOF while reading stdin, will quit now..");
			break;
		}

		delimit_by_spaces(option, &acc_id);

		if (option[0] == 'q')
			break;

		if (option[0] == 'h')
			pjsua_call_hangup_all();
    }

    /* Destroy pjsua */
	if (!didDestroy) {
		didDestroy = 1;
    	pjsua_destroy();
	}


    return 0;
}
/* 
TODO

 - Stop if not recognized sentence come in
 - Fix segmentation fault



*/
/*ERRORS


[2018/10/11 15:28:42:5682] ERR: ****** 0x2c006430: Sending new 648 (����+�<�+�<�+�<�+�), pending truncated ...
       It's illegal to do an lws_write outside of
       the writable callback: fix your code


	   ../src/pjmedia/conference.c:955: pjmedia_conf_connect_port: Assertion `conf && src_slot<conf->max_ports && sink_slot<conf->max_ports' failed.
15:45:32.919  pjsua_media.c  ......Audio updated, stream #0: PCMU (sendrecv)



Make Profile


welcome to Wells Fargo
Skip

but I said if you see any Spanien McKinley
Skip

for account access our questions say your account or card number or enter it followed by pound for anything else say other options
Dial -L -T number.txt

that was account number one two three right
Speak "yes"

please enter or say your account number or say I don't know it
Dial 1

I'm sorry I'm having trouble let me connect you with the banker who can help complete your request please hold this call maybe monitored or recorded

for account access say your account or card number one digit at a time orange red followed by pound now 

I'm sorry that's not a valid account number let's try again please enter or save the account number your account number can be found in your most recent statement or if you're calling about a checking account at the 

sorry please enter or say the account number one digit at a time 

please enter or say your account number or say I don't know it 

walking once for new shoes Michael how can I help you today 

I'm sorry 

*/