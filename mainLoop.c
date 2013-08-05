#include <ctl.h>
#include <msp430.h>
#include <stdlib.h>
#include "timerA.h"
#include "ARCbus.h"
#include "crc.h"
#include "spi.h"

#include "ARCbus_internal.h"

//bus internal events
CTL_EVENT_SET_t BUS_INT_events,BUS_helper_events;

//task structure for idle task and ARC bus task
CTL_TASK_t idle_task,ARC_bus_task,ARC_bus_helper_task;

//stack for ARC bus task
unsigned BUS_stack[256],helper_stack[100];

BUS_STAT arcBus_stat;

//events for subsystems
CTL_EVENT_SET_t SUB_events;

//address of SPI slave during transaction
static unsigned char SPI_addr=0;

static void ARC_bus_helper(void *p);

//ARC bus Task, do ARC bus stuff
static void ARC_bus_run(void *p) __toplevel{
  unsigned int e;
  unsigned char len;
  unsigned char addr,cmd;
  char resp;
  unsigned char pk[40];
  unsigned char *ptr;
  unsigned short crc;
  unsigned char *SPI_buf=NULL;
  ticker nt;
  int snd,i;
  SPI_addr=0;
  //first send "I'm on" command
  //BUS_cmd_init(pk,CMD_SUB_POWERUP);//setup command
  //send command
  //TODO: this seems to be causing problems when the CDH is not present
  //THIS NEEDS TO BE FIXED!!!!
  //resp=BUS_cmd_tx(BUS_ADDR_CDH,pk,0,0,SEND_FOREGROUND);
  #ifndef CDH_LIB         //Subsystem board 
    //check for failed send
    /*if(resp!=RET_SUCCESS){
      //wait a bit
      ctl_timeout_wait(ctl_get_current_time()+30);
      //resend
      resp=BUS_cmd_tx(BUS_ADDR_CDH,pk,0,0,SEND_FOREGROUND);
      //check for success
      if(resp!=RET_SUCCESS){
        //Failed
        #ifdef PRINT_DEBUG
          puts("Failed to detect CDH board\r");
        #endif
      }
    }*/
  #else     //CDH board, check for other CDH board
    if(resp==RET_SUCCESS){
      #ifdef PRINT_DEBUG
        puts("Other CDH board detected.\r");
      #endif
      //TODO : this is bad. perhaps do something here to recover
    }
  #endif
  //initialize helper events
  ctl_events_init(&BUS_helper_events,0);
  //start helper task
  ctl_task_run(&ARC_bus_helper_task,BUS_PRI_ARCBUS_HELPER,ARC_bus_helper,NULL,"ARC_Bus_helper",sizeof(helper_stack)/sizeof(helper_stack[0])-2,helper_stack+1,0);
  
  //event loop
  for(;;){
    //wait for something to happen
    e = ctl_events_wait(CTL_EVENT_WAIT_ANY_EVENTS_WITH_AUTO_CLEAR,&BUS_INT_events,BUS_INT_EV_ALL,CTL_TIMEOUT_NONE,0);
    //check if buffer can be unlocked
    if(e&BUS_INT_EV_BUFF_UNLOCK){
      SPI_buf=NULL;
      //unlock buffer
      BUS_free_buffer();
    }
    //check if I2C mutex can be released
    if(e&BUS_INT_EV_RELEASE_MUTEX){
      //release I2C mutex
      BUS_I2C_release();
    }
    //check if a SPI transaction is complete
    if(e&BUS_INT_EV_SPI_COMPLETE){
      //check if SPI was in progress
      if(SPI_addr){
        //turn off SPI
        SPI_deactivate();
        //tell helper thread to send SPI complete command
        ctl_events_set_clear(&BUS_helper_events,BUS_HELPER_EV_SPI_COMPLETE_CMD,0);
        //transaction complete, clear address
        SPI_addr=0;
        //assemble CRC
        crc=SPI_buf[arcBus_stat.spi_stat.len+1];//LSB
        crc|=(((unsigned short)SPI_buf[arcBus_stat.spi_stat.len])<<8);//MSB
        //check CRC
        if(crc!=crc16(SPI_buf,arcBus_stat.spi_stat.len)){
          //Bad CRC
          //clear buffer pointer
          SPI_buf=NULL;
          //free buffer
          BUS_free_buffer();
          //send event
          ctl_events_set_clear(&SUB_events,SUB_EV_SPI_ERR_CRC,0);
        }else{
          //tell subsystem, SPI data received
          //Subsystem must signal to free the buffer
          ctl_events_set_clear(&SUB_events,SUB_EV_SPI_DAT,0);
        }
      }
    }
    //check if an I2C command has been received
    if(e&BUS_INT_EV_I2C_CMD_RX){
      //=====================[I2C Command Received, Process command]===============================
      //clear response
      resp=0;
      //get len
      len=arcBus_stat.i2c_stat.rx.idx;
      //compute crc
      crc=crc8(i2c_buf,len-1);
      //get length of payload
      len=len-BUS_I2C_CRC_LEN-BUS_I2C_HDR_LEN;
      //get sender address
      addr=CMD_ADDR_MASK&i2c_buf[0];
      //get command type
      cmd=i2c_buf[1];
      //point to the first payload byte
      ptr=&i2c_buf[2];
      //check crc for packet
      if(ptr[len]==crc){
        //handle command based on command type
        switch(cmd){
          case CMD_SUB_ON:            
              //check for proper length
              if(len!=0){
                resp=ERR_PK_LEN;
              }
              //set new power status
              powerState=SUB_PWR_ON;
              //inform subsystem
              ctl_events_set_clear(&SUB_events,SUB_EV_PWR_ON,0);
          break;
          case CMD_SUB_OFF:
            //check to make sure that the command is directed to this subsystem
            if(len==1 && ptr[0]==UCB0I2COA){
              //set new power status
              powerState=SUB_PWR_ON;
              //inform subsystem
              ctl_events_set_clear(&SUB_events,SUB_EV_PWR_OFF,0);
            }else{
              //error with command
              resp=ERR_BAD_PK;
            }
          break;
          case CMD_SUB_STAT:
            //check for proper length
            if(len!=4){
              resp=ERR_PK_LEN;
            }
            //assemble time from packet
            nt=ptr[3];
            nt|=((ticker)ptr[2])<<8;
            nt|=((ticker)ptr[1])<<16;
            nt|=((ticker)ptr[0])<<24;
            //update time
            set_ticker_time(nt);
            //tell subsystem to send status
            ctl_events_set_clear(&SUB_events,SUB_EV_SEND_STAT,0);
          break;
          case CMD_RESET:          
            //check for proper length
            if(len!=0){
              resp=ERR_PK_LEN;
            }
            //cause a PUC by writing the wrong password to the watchdog
            WDT_RESET();
            //TODO: code should never get here, handle this if it does happen
            break;
          case CMD_SPI_RDY:
            //check length
            if(len!=2){
              resp=ERR_PK_LEN;
              break;
            }
            //assemble length
            arcBus_stat.spi_stat.len=ptr[1];//LSB
            arcBus_stat.spi_stat.len|=(((unsigned short)ptr[0])<<8);//MSB
            //check length account for 16bit CRC
            if(arcBus_stat.spi_stat.len+2>BUS_get_buffer_size()){
              //length is too long
              //cause NACK to be sent
              resp=ERR_SPI_LEN;
              break;
            }
            //check if already transmitting
            if(SPI_buf!=NULL){
              resp=ERR_SPI_BUSY;
              break;
            }
            SPI_buf=BUS_get_buffer(CTL_TIMEOUT_NOW,0);
            //check if buffer was locked
            if(SPI_buf==NULL){
              resp=ERR_BUFFER_BUSY;
              break;
            }
            //save address of SPI slave
            SPI_addr=addr;
            //TODO : Fill with actual data
            //fill  buffer with "random" data
            for(i=0;i<arcBus_stat.spi_stat.len;i++){
              SPI_buf[i]=i;
            }
            //calculate CRC
            crc=crc16(SPI_buf,arcBus_stat.spi_stat.len);
            //send CRC in big endian order
            SPI_buf[arcBus_stat.spi_stat.len]=crc>>8;
            SPI_buf[arcBus_stat.spi_stat.len+1]=crc;
            //setup SPI structure
            //TODO: make sure that buffer is not being used
            arcBus_stat.spi_stat.rx=SPI_buf;
            arcBus_stat.spi_stat.tx=SPI_buf;
            //Setup SPI bus to exchange data as master
            SPI_master_setup();
            //============[setup DMA for transfer]============
            //setup source trigger
            DMACTL0 &=~(DMA0TSEL_15|DMA1TSEL_15);
            DMACTL0 |= (DMA0TSEL_3|DMA1TSEL_4);
            // Source DMA address: receive register.
            DMA0SA = (unsigned short)(&UCA0RXBUF);
            // Destination DMA address: rx buffer.
            DMA0DA = (unsigned short)SPI_buf;
            // The size of the block to be transferred
            DMA0SZ = arcBus_stat.spi_stat.len+BUS_SPI_CRC_LEN;
            // Configure the DMA transfer, single byte transfer with destination increment
            DMA0CTL = DMAIE|DMADT_0|DMASBDB|DMAEN|DMADSTINCR1|DMADSTINCR0;
            // Source DMA address: tx buffer
            //skip the second byte, first byte sent manually
            DMA1SA = (unsigned int)(arcBus_stat.spi_stat.tx+1);
            // Destination DMA address: the transmit buffer.
            DMA1DA = (unsigned int)(&UCA0TXBUF);
            // The size of the block to be transferred
            DMA1SZ = arcBus_stat.spi_stat.len+BUS_SPI_CRC_LEN-1;
            // Configure the DMA transfer, single byte transfer with source increment
            DMA1CTL=DMADT_0|DMASBDB|DMAEN|DMASRCINCR1|DMASRCINCR0;
            //write first byte into the Tx buffer to start transfer
            UCA0TXBUF=*arcBus_stat.spi_stat.tx;
          break;
          
          case CMD_SPI_COMPLETE:
            //turn off SPI
            SPI_deactivate();
            //SPI transfer is done
            //notify CDH board
            //TODO: some sort of error check to make sure that a SPI transfer was requested and send an ERROR if one was not
#ifndef CDH_LIB
            ctl_events_set_clear(&BUS_helper_events,BUS_HELPER_EV_SPI_CLEAR_CMD,0);
#endif
            //notify calling task
            ctl_events_set_clear(&arcBus_stat.events,BUS_EV_SPI_COMPLETE,0);
          break;
          case CMD_ASYNC_SETUP:
            //check length
            if(len!=1){
              resp=ERR_PK_LEN;
              break;
            }
            switch(ptr[0]){
              case ASYNC_OPEN:
                //open remote connection
                async_open_remote(addr);
              break;
              case ASYNC_CLOSE:
                //check if sending address corosponds to async address
                if(async_addr!=addr){
                  //ERROR : not open
                  //TODO: do something here
                  //resp=
                  break;
                }
                //tell helper thread to close connection
                ctl_events_set_clear(&BUS_helper_events,BUS_HELPER_EV_ASYNC_CLOSE,0);
              break;
              case ASYNC_STOP:
                //check that async address is sender address
                if(addr!=async_addr){
                  //TODO: handle error here
                  break;
                }
                //stop async from sending
                txFlow=ASYNC_FLOW_STOPPED;
                //TESTING: set LED
                P7OUT|=BIT1;
              break;
              case ASYNC_RESTART:
                //check that async address is sender address
                if(addr!=async_addr){
                  //TODO: handle error here
                  break;
                }
                //restart async sending
                txFlow=ASYNC_FLOW_RUNNING;
                //TESTING: set LED
                P7OUT&=~BIT1;
                //tell helper to send data
                ctl_events_set_clear(&BUS_helper_events,BUS_HELPER_EV_ASYNC_SEND,0);
              break;
              default:
                //unknown command: handle accordingly
                //TODO: handle this better
                __no_operation();
            }
          break;
          case CMD_ASYNC_DAT:
            //TODO: check sender address
            //post bytes to queue
            ctl_byte_queue_post_multi_nb(&async_rxQ,len,ptr);
            //TESTING: toggle LED
            P7OUT^=BIT3;
            //check if restarting
            if(rxFlow==ASYNC_FLOW_RESTARTING){
              //flow is running, packet recieved
              rxFlow=ASYNC_FLOW_RUNNING;
            }
            //check free bytes in queue
            if(rxFlow!=ASYNC_FLOW_OFF && ctl_byte_queue_num_free(&async_rxQ)<=ASYNC_FLOW_STOP_THRESHOLD){              
              //tell helper thread to send stop command
              ctl_events_set_clear(&BUS_helper_events,BUS_HELPER_EV_ASYNC_STOP,0);
            }
          break;
          case CMD_NACK:
            //TODO: handle this better somehow?
            //set event 
            ctl_events_set_clear(&arcBus_stat.events,BUS_EV_CMD_NACK,0);
            #ifdef PRINT_DEBUG
              //TESTING: print message
              printf("\r\nNACK recived for command %i with reason %i\r\n",ptr[0],ptr[1]);
            #endif
          break;
          default:
            //check for subsystem command
            resp=SUB_parseCmd(addr,cmd,ptr,len);
          break;
        }
        //malformed command, send nack if requested
        if(resp!=0 && i2c_buf[0]&CMD_TX_NACK){
          #ifdef PRINT_DEBUG
            printf("Error : resp %i\r\n",(char)resp);
          #endif
          //setup command
          ptr=BUS_cmd_init(pk,CMD_NACK);
          //send NACK reason
          ptr[0]=resp;
          //send packet
          BUS_cmd_tx(addr,pk,1,0,BUS_I2C_SEND_BGND);
        }
      }else if(cmd!=CMD_NACK){
        //CRC check failed, send NACK
        #ifdef PRINT_DEBUG
          printf("Error : bad CRC Command %i\r\nRecived CRC 0x%02X calculated CRC 0x%02X\r\n",cmd,ptr[len],crc);
          //print out packet
          for(i=0;i<len;i++){
            printf("0x%02X ",ptr[i]);
          }
        #endif
        //setup command
        ptr=BUS_cmd_init(pk,CMD_NACK);
        //send NACK reason
        ptr[0]=ERR_BAD_CRC;
        //send packet
        BUS_cmd_tx(addr,pk,1,0,BUS_I2C_SEND_BGND);
      }
    }
  }
}
    
    
//ARC bus Task, do ARC bus stuff
static void ARC_bus_helper(void *p) __toplevel{
  unsigned int e;
  int resp;
  unsigned num;
  unsigned char *ptr,pk[BUS_I2C_HDR_LEN+2+BUS_I2C_CRC_LEN];
  for(;;){
    e=ctl_events_wait(CTL_EVENT_WAIT_ANY_EVENTS_WITH_AUTO_CLEAR,&BUS_helper_events,BUS_HELPER_EV_ALL,CTL_TIMEOUT_NONE,0);
    if(e&BUS_HELPER_EV_SPI_COMPLETE_CMD){      
      //done with SPI send command
      BUS_cmd_init(pk,CMD_SPI_COMPLETE);
      resp=BUS_cmd_tx(SPI_addr,pk,0,0,BUS_I2C_SEND_FOREGROUND);
      //check if command was successful and try again if it failed
      if(resp!=RET_SUCCESS){
        resp=BUS_cmd_tx(SPI_addr,pk,0,0,BUS_I2C_SEND_FOREGROUND);
      }
      //check if command sent successfully
      if(resp!=RET_SUCCESS){
        //TODO: report error
      }
    }
    if(e&BUS_HELPER_EV_SPI_CLEAR_CMD){
      //done with SPI send command
      BUS_cmd_init(pk,CMD_SPI_CLEAR);
      resp=BUS_cmd_tx(BUS_ADDR_CDH,pk,0,0,BUS_I2C_SEND_FOREGROUND);
      //check if command was successful and try again if it failed
      if(resp!=RET_SUCCESS){
        resp=BUS_cmd_tx(BUS_ADDR_CDH,pk,0,0,BUS_I2C_SEND_FOREGROUND);
      }
      //check if command sent successfully
      if(resp!=RET_SUCCESS){
        //TODO: report error
      }
    }
    if(e&BUS_HELPER_EV_ASYNC_CLOSE){      
      //close async connection
      async_close_remote();
      //send event
      ctl_events_set_clear(&SUB_events,SUB_EV_ASYNC_CLOSE,0);
    }
    if(e&BUS_HELPER_EV_ASYNC_STOP){
      //stop async from sending
      ptr=BUS_cmd_init(pk,CMD_ASYNC_SETUP);
      ptr[0]=ASYNC_STOP;
      BUS_cmd_tx(async_addr,pk,1,0,BUS_I2C_SEND_FOREGROUND);
      if(resp==RET_SUCCESS){
        //command sent, track status
        rxFlow=ASYNC_FLOW_STOPPED;  
        //TESTING: set LED
        P7OUT|=BIT2;
      }
    }
    //async timer timed out, send data
    //do this last because it will restart if there is more data to send
    if(e&BUS_HELPER_EV_ASYNC_SEND){
      //send some data
      async_send_data();
      num=ctl_byte_queue_num_used(&async_txQ);
      if(num>ASYNC_TARGET_SIZE){
        //send more data
        ctl_events_set_clear(&BUS_helper_events,BUS_HELPER_EV_ASYNC_SEND,0);
      }else if(num!=0 && async_timer==0){
        //there are a few chars left, reset timer
        async_timer=30;
      }
    }
  }
}

//=======================================================================================
//                                 [Main Loop]
//=======================================================================================

//main loop function, start ARC_Bus task then enter Idle task
void mainLoop(void) __toplevel{
  //initialize events
  ctl_events_init(&BUS_INT_events,0);
  //start ARCbus task
  ctl_task_run(&ARC_bus_task,BUS_PRI_ARCBUS,ARC_bus_run,NULL,"ARC_Bus",sizeof(BUS_stack)/sizeof(BUS_stack[0])-2,BUS_stack+1,0);
  //kick WDT to give us some time
  WDT_KICK();
  // drop to lowest priority to start created tasks running.
  ctl_task_set_priority(&idle_task,0); 
  
  //main idle loop
  //NOTE that this task should never wait to ensure that there is always a runnable task
  for(;;){    
      //kick watchdog
      WDT_KICK();
      #ifndef IDLE_DEBUG
        //go to low power mode
        LPM0;
      #else
        P7OUT^=BIT0+BIT1+BIT2+BIT3;
      #endif
  }
}
