/*
 * Copyright 2007-2010 National ICT Australia (NICTA), Australia
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <log.h>
#include <ocomm/o_socket.h>
#include <ocomm/o_eventloop.h>

#include "proxy_client_handler.h"

#define DEF_TABLE_COUNT 10

extern ProxyServer* proxyServer;
/**
 * \brief Create and initialise a new +ProxyClientBuffer+ structure
 * \param size the maximum lenght of the buffer
 * \param number the page number
 * \return
 */
ProxyClientBuffer* initPCB( int size, int number){
    ProxyClientBuffer* self = (ProxyClientBuffer*) malloc(sizeof(ProxyClientBuffer));
    memset(self, 0, sizeof(ProxyClientBuffer));
    self->max_length = size;
    self->pageNumber = number;
    self->currentSize = 0;
    self->byteAlreadySent = 0;
    self->buff = (unsigned char*) malloc(size*sizeof(char));

    memset(self->buff, 0, size*sizeof(char));
    self->buffToSend = self->buff;
    self->current_pointer = self->buff;
    self->next = NULL;

    return self;
}


static void
client_callback(SockEvtSource* source, void* handle, void* buf, int buf_size);

/* errno is not a good name for an variable as it expands to a fn ptr
   when unistd.h is included */
static void
status_callback(SockEvtSource* source, SocketStatus status, int err_no,
                void* handle);

/**
 * \brief function that will try to send data to the real OML server
 * \param handle the proxy Client Handler
 */
static void* thread_proxystart(void* handle) {

  ProxyClientHandler* client = ( ProxyClientHandler* ) handle;
  while(1){
    ProxyClientBuffer* buffer = client->currentBuffertoSend;
    switch (proxyServer->state) {
    case ProxyState_SENDING:
      if((buffer->currentSize - buffer->byteAlreadySent)>0){
        if(socket_sendto(client->socket_to_server,
                         (char*)buffer->buffToSend,
                         (buffer->currentSize - buffer->byteAlreadySent))==0) {
          buffer->buffToSend += (buffer->currentSize - buffer->byteAlreadySent);
          buffer->byteAlreadySent = buffer->currentSize;
        }
      }
      if(buffer->next != NULL){
        client->currentBuffertoSend = buffer->next;
      }
      else{
        if(client->socketClosed == 1){
          socket_close(client->socket_to_server);
          client->socketClosed++;  // Inform the status callback that we closed our end.
        }
      }
      break;
    case ProxyState_STOPPED:
      break;
    case ProxyState_PAUSED:
      sleep (1);
      break;
    default:
      logerror ("Unknown ProxyState %d\n", proxyServer->state);
    }
  }
}

/**
 * \brief Create and initialise a +ProxyClientHandler+ structure
 * \param newSock the socket associated to the client transmition
 * \param size_page the size of the buffers
 * \param file_name the name of the file to save the measurements
 * \param portServer the destination port of the OML server
 * \param addressServer the address of the OML Server
 * \return a new ProxyClientHandler structure
 */
ProxyClientHandler*
proxy_client_handler_new(
    Socket* newSock,
    int size_page,
    char* file_name,
    int portServer,
    char* addressServer
) {
  ProxyClientHandler* self = (ProxyClientHandler *)malloc(sizeof(ProxyClientHandler));
  memset(self, 0, sizeof(ProxyClientHandler));

  self->socket = newSock;
  self->buffer = initPCB( size_page, 0); //TODO change it to integrate option of the command line
  self->firstBuffer = self->buffer;
  self->currentBuffertoSend = self->buffer;
  self->currentPageNumber = 0;
  self->file = fopen(file_name, "wa");
  self->socketClosed = 0;

  self->file_name =  file_name;
  self->addressServer = addressServer;
  self->portServer = portServer;

  self->socket_to_server =  socket_tcp_out_new(file_name, addressServer, portServer);

  pthread_create(&self->thread, NULL, thread_proxystart, (void*)self);

  return self;
  //eventloop_on_read_in_channel(newSock, client_callback, status_callback, (void*)self);
}

void
startLoopChannel(Socket* newSock, ProxyClientHandler* proxy){
  eventloop_on_read_in_channel(newSock, client_callback, status_callback, (void*)proxy);
}

/**
 * \brief function called when the socket receive some data
 * \param source the socket event
 * \param handle the cleint handler
 * \param buf data received from the socket
 * \param bufsize the size of the data set from the socket
 */
void
client_callback(
  SockEvtSource* source,
  void* handle,
  void* buf,
  int buf_size
) {
  ProxyClientHandler* self = (ProxyClientHandler*)handle;
  int available = self->buffer->max_length - self->buffer->currentSize ;

  if(self->file == NULL)
      ;
  else{
    if(available < buf_size){
        fwrite(self->buffer->buff,sizeof(char), self->buffer->currentSize, self->file);
        //fflush(self->queue);
        //socket_sendto(self->socket_to_server,self->buffer->buff,self->buffer->currentSize);

        self->currentPageNumber += 1;
        self->buffer->next = initPCB(self->buffer->max_length, self->currentPageNumber);
        self->buffer = self->buffer->next;

        //memcpy(self->buffer->current_pointer, buf, buf_size);

        //self->buffer->current_pointer += buf_size;
        //self->buffer->currentSize += buf_size;

    }



    memcpy(self->buffer->current_pointer,  buf, buf_size);
    self->buffer->current_pointer += buf_size;
    self->buffer->currentSize += buf_size;


  }

}

/**
 * \brief Call back function when the status of the socket change
 * \param source the socket event
 * \param status the status of the socket
 * \param errno the value of the error if there is
 * \param handle the Client handler structure
 */
void
status_callback(
 SockEvtSource* source,
 SocketStatus status,
 int err_no,
 void* handle
) {
  switch (status) {
    case SOCKET_CONN_CLOSED: {
      ProxyClientHandler* self = (ProxyClientHandler*)handle;
      fwrite(self->buffer->buff,sizeof(char), self->buffer->currentSize, self->file);
      fflush(self->file);
      fclose(self->file);
      self->socketClosed = 1;

      /*
       * The thread that sends to the downstream oml2-server must
       * detect that socketClosed == 1 and increment it to 2 to inform
       * this thread that it is ok to declare the socket closed and
       * exit.
       *
       * FIXME:  This blocks the main event loop for a second at a time.
       */
      while(self->socketClosed <= 1)
        sleep(1);
      logdebug("socket '%s' closed\n", source->name);
      break;
    default:
      break;
    }
  }
}

/*
 Local Variables:
 mode: C
 tab-width: 4
 indent-tabs-mode: nil
 End:
*/
