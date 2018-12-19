/* 
 * File:   receiver_main.c
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
#define MAXBUFLEN 8000
#define recv_window_size 10
#define header_size 13
struct sockaddr_in s_addr_me, si_other;
int FIN;
int syn_ack_heard = 0;
int sockfd;
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
/*
    Helper to calculate index in receiving window
*/
int indexr(int in){
    return in>=0?(in%recv_window_size):(recv_window_size-1);
}
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

int send_helper (rec_pkt_t* send_str){
    int numbytes;
    if ((numbytes = sendto(sockfd, (char*)send_str, sizeof(rec_pkt_t), 0,(struct sockaddr *)&si_other, sizeof(si_other)) ) == -1){
        perror("talker: sendto");
        exit(1);
    }
    printf("Sending packet %c %llu\n",send_str->type, send_str->seq_no);
    return numbytes;
}

/*
    Helper to receive packet
*/
void listen_to(send_pkt_t* buf){
    socklen_t addr_len;
    int numbytes;
    addr_len = sizeof si_other;

    while(1){
        if ((numbytes = recvfrom(sockfd, (char*)buf, sizeof(send_pkt_t) , 0, (struct sockaddr *)&si_other, &addr_len)) != sizeof(send_pkt_t)) {
            printf("RECEIVER: fragment dropped\n");
            continue;
        }
        break;
    }
    printf("Heard packet %c %llu %d\n",buf->type, buf->seq_no, buf->packet_size);
    return;
}



void *counter_thread_fun(){
    rec_pkt_t send_buf;
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
int create_UDP_connection(int myUDPport){
    send_pkt_t recv_buf;
    rec_pkt_t send_buf;
    pthread_t counter_thread;

    if ((sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
        diep("socket");

    memset((char *) &s_addr_me, 0, sizeof (s_addr_me));
    s_addr_me.sin_family = AF_INET;
    s_addr_me.sin_port = htons(myUDPport);
    s_addr_me.sin_addr.s_addr = htonl(INADDR_ANY);
    printf("Now binding\n");
    if (bind(sockfd, (struct sockaddr*) &s_addr_me, sizeof (s_addr_me)) == -1)
        diep("bind");
    printf("Finish binding...\n");

    printf("Wait for handshake...\n");
    while(1){
        listen_to(&recv_buf);
        if((recv_buf.type!='S')||(recv_buf.seq_no!=0))
            continue;
        else
            break;
    }
    printf("SYN heard...\n");  

    send_buf.type = 'S'; 
    send_buf.seq_no = 0;
    send_helper(&send_buf);
    printf("SYN.ACK sent...\n");    
    //counter thread
    pthread_create(&counter_thread, NULL, counter_thread_fun, NULL);
    syn_ack_heard = 0;
    listen_to(&recv_buf);
    syn_ack_heard = 1;
    printf("Finish handshake...\n");  

    return sockfd;
}

/*
    Helper to finish TCP handshake
*/
void finish_handshake(){;
    rec_pkt_t send_buf; 
    send_buf.type = 'F'; 
    send_buf.seq_no = 0;
    send_helper(&send_buf);
    return;
}

/*
    Main function for TCP reliable receive
*/
void reliablyReceive(unsigned short int myUDPport, char* destinationFile) {
    send_pkt_t recv_buf;
    rec_pkt_t send_buf;
    int i;
    FILE* fp;

    int recv_offset = 0;

    char recv_DATA[recv_window_size][MAXBUFLEN-header_size];

    int recv_DATA_len[recv_window_size];

    int valid_flag[recv_window_size];

    unsigned long long int recv_seq_no[recv_window_size];

    for(i = 0; i< recv_window_size; i++){
        recv_seq_no[i] = i;
        valid_flag[i] = 0;
        recv_DATA_len[i] = 0;
    }

    FIN = 0;


//FILE Options
    fp = fopen(destinationFile, "a");

//Initialize connection and handshake

    create_UDP_connection(myUDPport);
    if(sockfd==-1)
        diep("Error HANDSHAKING");

/* Now receive data and send acknowledgements */ 
    while(!FIN){

        listen_to(&recv_buf);

        //If FIN
        if(recv_buf.type=='F'){
            puts("Going to finish...\n\n");
            finish_handshake();
            FIN = 1;
            //clean up stuff left in buf

            for(i = recv_offset; i != recv_offset ; i = indexr(i+1) ){
                if(valid_flag[i])
                    fwrite(recv_DATA[i], 1, recv_DATA_len[i], fp);
            }
            puts("FINISH.\n");
            continue;
        }
/*
        1. Check if not D, drop it
        2. check seq_no, if recv_offset<= seq_no <= recv_offset-1 then fill buf
        3. If shiftable, shift
        4. send back most recent ackable packet
*/
//1.
        if(recv_buf.type!='D') continue;

//2.
        if((recv_buf.seq_no>=recv_seq_no[recv_offset])&&
            (recv_buf.seq_no<=recv_seq_no[indexr(recv_offset-1)])){

            i = indexr(recv_offset + (recv_buf.seq_no - recv_seq_no[recv_offset]));

            if(!valid_flag[ i ]){
                recv_DATA_len[ i ] = recv_buf.packet_size;
                memcpy(recv_DATA[ i ], recv_buf.data, recv_buf.packet_size);
                valid_flag[ i ] = 1;
            }

            valid_flag[ i ] = 1;
        }

//3. 
        {
            i = recv_offset;
            while(1){
        
                if(valid_flag[i] == 0) break;
    //A. clear valid flag
                valid_flag[i] = 0;
    //B. update seq_no array
                recv_seq_no[i] = recv_seq_no[indexr(i-1)] + 1;
                printf("%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu\n", recv_seq_no[0], recv_seq_no[1], recv_seq_no[2], recv_seq_no[3], recv_seq_no[4], recv_seq_no[5], recv_seq_no[6], recv_seq_no[7], recv_seq_no[8], recv_seq_no[9]);
    //C. Write data back into file
                fwrite(recv_DATA[i], 1, recv_DATA_len[i], fp);

                i = indexr(i+1);
            }
            recv_offset = i;
        }

//4.
        send_buf.type = 'A'; 

        send_buf.seq_no = recv_seq_no[recv_offset] - 1;

        send_helper(&send_buf);

    }

    /* Finish recovering file*/
    close(sockfd);
    fclose(fp);
	printf("%s received.", destinationFile);
    return;
}

/*
 * 
 */
int main(int argc, char** argv) {

    unsigned short int udpPort;

    if (argc != 3) {
        fprintf(stderr, "usage: %s UDP_port filename_to_write\n\n", argv[0]);
        exit(1);
    }

    udpPort = (unsigned short int) atoi(argv[1]);

    reliablyReceive(udpPort, argv[2]);
}

