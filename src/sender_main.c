/* 
 * File:   sender_main.c
 * Author: Tianyi Tang
 *
 * Created on 2018/10/29 
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <signal.h>
#include <string.h>
#include <sys/time.h>
#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#define MAXBUFLEN 8000
#define header_size 13
//TODO 1. Potential bug: fill_cw is slow
typedef struct rec_pkt_t{
    char type;
    unsigned long long int seq_no;
}rec_pkt_t;

typedef struct send_pkt_t{
    char type;
    unsigned long long int seq_no;
    int packet_size;
    char data[MAXBUFLEN-header_size];
}send_pkt_t;

typedef struct send_pkt_ll{
    send_pkt_t* data;
    unsigned long long int timeout;//in u sec
    struct timeval start;
    //gettimeofday()
    int sent;
    struct send_pkt_ll* next;   
}send_pkt_ll;

int sockfd;
FILE *fp;
int syn_ack_heard = 0;
unsigned long long int init_timeout = 50000;
int file_finish = 0;
unsigned long long int bytes_transfered = 0;
unsigned long long int bytes_to_transfered = 0;
struct sockaddr_in si_other;
send_pkt_ll* cw;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
int cwnd = 1;
int dupACKcnt = 0;
int ssthreshold = 128;
/*
    0 ---- slow start
    1 ---- congestion avoidance
    2 ---- fast recovery
*/
int stage = 0;
/*
    Helper to raise fault
*/
void diep(char *s) {
    perror(s);
    exit(1);
}
/*
    Helper to send packet
*/

int send_helper (send_pkt_t* send_buf){
    int numbytes;
    while(1){
        if ((numbytes = sendto(sockfd, (char*)send_buf, sizeof(send_pkt_t), 0,
                 (struct sockaddr *)&si_other, sizeof(si_other)) ) != sizeof(send_pkt_t)){
            printf("SENDER: fragment dropped\n");
            continue;
        }
        break;
    }
    printf("Sending packet %c %llu %d\n",send_buf->type, send_buf->seq_no, cwnd);
    return numbytes;
}

/*
    Helper to receive packet
*/
void listen_to(rec_pkt_t* buf){
    socklen_t addr_len;
    int numbytes;

    addr_len = sizeof si_other;
    printf("Listening....");
    if ((numbytes = recvfrom(sockfd, (char*)buf, sizeof(rec_pkt_t) , 0, (struct sockaddr *)&si_other, &addr_len)) == -1) {
        perror("recvfrom");
        exit(1);
    }
    printf("Heard packet %c %llu %d stage %d\n",buf->type, buf->seq_no, cwnd, stage);
    return;
}



void *counter_thread_fun(){
    send_pkt_t send_buf;
    send_buf.type = 'S'; 
    send_buf.seq_no = 0;
    while(1){
        usleep(30000);
        if(syn_ack_heard) break;
        send_helper(&send_buf);
    }
    return NULL;
}
/*
    Helper to create connection and finish handshake
*/
void create_UDP_connection(char* hostname, unsigned short int hostUDPport){

    rec_pkt_t recv_buf;
    send_pkt_t send_buf;
    pthread_t counter_thread;

    if ((sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
        diep("socket");

    memset((char *) &si_other, 0, sizeof (si_other));
    si_other.sin_family = AF_INET;
    si_other.sin_port = htons(hostUDPport);
    if (inet_aton(hostname, &si_other.sin_addr) == 0) {
        fprintf(stderr, "inet_aton() failed\n");
        exit(1);
    }
    printf("Start handshake...\n");

//SYN...
    send_buf.type = 'S'; 
    send_buf.seq_no = 0;
    send_helper (&send_buf);
    printf("SYN...\n");
    syn_ack_heard = 0;
//SYNACK..
    pthread_create(&counter_thread, NULL, counter_thread_fun, NULL);

    listen_to(&recv_buf);
    if((recv_buf.type!='S')||(recv_buf.seq_no!=0))
        diep("SYNACK error");
    printf("SYN/ACK heard...\n");
    syn_ack_heard = 1;
//ACK...
    send_buf.type = 'B'; 
    send_buf.seq_no = 1;
    send_helper (&send_buf);
    printf("FINISH handshake...\n");

    return;
}

/*
    Helper to finish TCP handshake
*/
void finish_handshake(){
    int i = 0;
    send_pkt_t send_buf; 
    send_buf.type = 'F'; 
    send_buf.seq_no = 0;
    send_helper(&send_buf);

    puts("Finish sending fin waiting for ack...\n");
    
    while(i<15){
        send_helper(&send_buf);
        i++;
    }

    return;
}

/*
    Thread to send window packet
*/
void *send_thread_fun(){
    send_pkt_ll* counter;
    struct timeval now;
    int i = 0;
    unsigned long long int cnt = 0;
    pthread_mutex_lock(&lock);
    while(!cw){
	pthread_mutex_unlock(&lock);
	 usleep(20);
	pthread_mutex_lock(&lock);
    }
    pthread_mutex_unlock(&lock);
    while(1){
        cnt++;
        pthread_mutex_lock(&lock);
        if(cwnd<1){
            pthread_mutex_unlock(&lock);
            break;    
        }
        pthread_mutex_unlock(&lock);

        
/*
        1. go through the linked list, send any unsent packet, update its timeout, update sent status
        2. if timeout, update window and state machine, resend missing segment
        p.s: careful with critical section on shared variable
*/
        pthread_mutex_lock(&lock);
        counter = cw;
        for(i = 0; i<cwnd ; i++){
    	    if(!counter){
                break;
            }
            if(!(counter->sent)){
                cnt = 0;
                send_helper(counter->data);
                counter->sent = 1;
                gettimeofday(&(counter->start), NULL);
            }

            else{
                gettimeofday(&(now), NULL);
                if(cnt>400337){
                    printf("cur time is %d, start is %d, diff is %d cnt is %llu \n",(now.tv_usec),((counter->start).tv_usec),(now.tv_usec-(counter->start).tv_usec), cnt);
                }
                if((now.tv_usec - (counter->start).tv_usec) > (counter->timeout) ){
                    cnt = 0;
                    printf("Timeout Happened!\n");
                    counter->start = now;
                    counter->sent = 1;
                    ssthreshold = ((int)(cwnd/2))>0?((int)(cwnd/2)):1;
                    cwnd = 1;
                    dupACKcnt = 0;
                    stage = 0;
                    send_helper(counter->data);
                    break;
                }

            }
            counter = counter -> next;
        }
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

/*
    Thread to recv window ack and move state machine
*/
void fill_cw(int seq_no){
    send_pkt_ll* ptr = cw;
    send_pkt_ll* prev = NULL;
    send_pkt_t * new_packet;
    int size;
    int i;
    for(i = 0; i<cwnd ; i++)
    {
        if(!ptr){

            new_packet = (send_pkt_t*) malloc(sizeof(send_pkt_t));
            new_packet->type = 'D';
            new_packet->seq_no = seq_no;
            if((size = fread((new_packet->data), 1, 
                MIN((MAXBUFLEN-header_size),(bytes_to_transfered - bytes_transfered)),fp))
                < (MAXBUFLEN-header_size) ){
                cwnd = i+1;
                file_finish = 1;
            }
            bytes_transfered += size;

            new_packet->packet_size = size;

            ptr = (send_pkt_ll*) malloc(sizeof(send_pkt_ll));
            ptr->data = new_packet;
            ptr->timeout = init_timeout;
            ptr->sent = 0;
            ptr->next = NULL;
            if(prev)
                prev->next = ptr;
	    else{
		cw = ptr;
	    }
        }
        prev = ptr;
        ptr = ptr->next;
        seq_no++;
    }
    return;
}


/*
    Thread to recv window ack and move state machine
*/
void clear_cw(int shift_amount){
    int i;
    send_pkt_ll* temp;
    for(i = 0; i < shift_amount ; i++){
        free(cw->data);
        temp = cw;
        cw = cw -> next;
        free(temp);
    }
    return;
}

/*
    Thread to recv window ack and move state machine
*/
void *recv_thread_fun(){

    rec_pkt_t recv_buf;
    int seq_no = 0;
    int cumulator = 0;
    int shift_amount = 0;
    pthread_mutex_lock(&lock);
    fill_cw(0);
    pthread_mutex_unlock(&lock);
    while(1){
        pthread_mutex_lock(&lock);
        if(cwnd<1){
            pthread_mutex_unlock(&lock);
            break;    
        }
        pthread_mutex_unlock(&lock);

        listen_to(&recv_buf);
        seq_no = recv_buf.seq_no;
        if(recv_buf.type!='A')
            continue;

        pthread_mutex_lock(&lock);        
        if((seq_no == (cw->data)->seq_no-1)&&(!file_finish)){

            dupACKcnt = (dupACKcnt+1)>3?3:(dupACKcnt+1);

            if(dupACKcnt==3){
                // dupACKcnt 到3了 cut ssthreshold, cwnd update, go 2
                printf("DupACK Happened!\n");
                if(stage==2){

                    cwnd++;

                    fill_cw(seq_no+1);
                }
                else{

                    ssthreshold = (int)(cwnd/2);

                    cwnd = ssthreshold + 3;

                    fill_cw(seq_no+1);
                }

            }
        }
        else if(seq_no >= (cw->data)->seq_no){
            shift_amount = seq_no - (cw->data)->seq_no + 1;
            clear_cw(shift_amount);
            if(file_finish){
                cwnd -= shift_amount;
            }
            else{
                switch(stage){
                    case 0:
                    //    FIRST update cwnd cw, clear dupACKcount, if cwnd >=ssthreshold go 1
                        dupACKcnt = 0;
                
                        cwnd += shift_amount;
                    
                        fill_cw(seq_no+1);

                        if(cwnd >= ssthreshold)
                            stage = 1;

                        cumulator = 0;

                    break;

                    case 1:
                    //    SECOND update cwnd cw (smaller), clear dupACKcount, update cumulator
                        dupACKcnt = 0;

                        cumulator+=shift_amount;

                        if(cumulator == cwnd){
                            cwnd++;

                            cumulator = 0;
                        }

                        fill_cw(seq_no+1);
                    break;
                    case 2:
                    //    THIRD update cwnd cw, clear dupACKcount

                        dupACKcnt = 0;

                        cwnd = ssthreshold;

                        cumulator = 0;

                        fill_cw(seq_no+1);
                    break;
                }
            }
        }
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}


/*
    Main function for TCP reliable transfer
*/
void reliablyTransfer(char* hostname, unsigned short int hostUDPport, char* filename, unsigned long long int bytesToTransfer) {

    //Open the file
    pthread_t send_thread, recv_thread; 
    fp = fopen(filename, "rb");
    if (fp == NULL) {
        printf("Could not open file to send.");
        exit(1);
    }
    bytes_to_transfered = bytesToTransfer;
    create_UDP_connection(hostname, hostUDPport);

    
	/* Send data and receive acknowledgements on s*/
    pthread_create(&recv_thread, NULL, recv_thread_fun, NULL);
    pthread_create(&send_thread, NULL, send_thread_fun, NULL);
    puts("Two main thread initialized...\n");
    pthread_join(send_thread, NULL);
    pthread_join(recv_thread, NULL);
    //FIN
    puts("Fin handshaking\n");
    finish_handshake();
    printf("Closing the socket\n");
    close(sockfd);
    return;
}

/*
 * 
 */
int main(int argc, char** argv) {

    unsigned short int udpPort;
    unsigned long long int numBytes;

    if (argc != 5) {
        fprintf(stderr, "usage: %s receiver_hostname receiver_port filename_to_xfer bytes_to_xfer\n\n", argv[0]);
        exit(1);
    }
    udpPort = (unsigned short int) atoi(argv[2]);
    numBytes = atoll(argv[4]);



    reliablyTransfer(argv[1], udpPort, argv[3], numBytes);


    return (EXIT_SUCCESS);
}


