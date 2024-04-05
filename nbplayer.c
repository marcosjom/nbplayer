//
//  nbplayer.c
//
//
//  Created by Marcos Ortega on 11/3/24.
//

#define K_DEBUG             //if defined, internal debug code is enabled
//#define K_DEBUG_VERBOSE   //if defined, a lot of text is printed.
#define K_USE_MPLANE        //if defined, _MPLANE buffers are used instead of single-plane (NOTE: '_MPLANE' seems not to work with G_CROP ctl)

//

#include <stdio.h>          //printf and generic calls
#include <stdlib.h>         //for "abort" (NBASSERT) and "srand()", "rand()"
#include <stdint.h>         //for uint32_t
#include <linux/version.h>  //for KERNEL_VERSION() macro
#include <linux/videodev2.h>
#include <sys/ioctl.h>      //for ioctl
#include <sys/mman.h>       //for mmap
#include <string.h>         //for memset
#include <fcntl.h>          //for open
#include <unistd.h>         //for open/red/write
#include <errno.h>          //for EAGAIN
#include <pthread.h>        //thread, mutex and cond
#include <unistd.h>         //for usleep()
#include <libv4l2.h>        //for v4l2_* calls, gives support to v4l1 drivers and extra pixel formats, https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/libv4l-introduction.html
                            //requires '-lv4l2' at compilation.
//socket
#include <unistd.h>         //for close() de sockets
#include <sys/socket.h>     //for socklen_t
#include <netinet/in.h>     //for sockaddr_in
#include <netinet/tcp.h>    //for TCP_NODELAY (also in /usr/include/linux/tcp.h or /usr/include/netinet/tcp.h)
#include <arpa/inet.h>      //for structs
#include <netdb.h>          //for hostent
#include <errno.h>          //for errno
#include <string.h>         //for strerror()
#include <fcntl.h>          //for O_NONBLOCK
#include <poll.h>           //for poll()
#include <signal.h>         //for signal() and interrputs

//
#ifdef K_DEBUG
#   include <assert.h>      //for assert()
#   define  IF_DEBUG(V)     V
#   define  K_ASSERT(V)     if(!(V)){ printf("Assert failed line %d, func '%s': '%s'\n", __LINE__, __func__, #V); assert(0); exit(-1); }
#else
#   define  IF_DEBUG(V)     //empty
#   define  K_ASSERT(V)     //empty
#endif

//framebuffer

#include <linux/fb.h>

//default values

#define K_DEF_REPRINTS_HIDE_SECS    (60 * 60) //redundant info printing skip time (like devices properties, once printed, wait this ammount of secs before printing them again).
#define K_DEF_THREADS_EXTRA_AMM     0       //ammount of extra threads (for rendering). Note: best efficiency is '0 extra threads' (single thread), best performance is '1 extra thread' dual-threads.
#define K_DEF_CONN_TIMEOUT_SECS     60      //seconds to wait for connection-inactivity-timeout.
#define K_DEF_CONN_RETRY_WAIT_SECS  5       //seconds to wait before trying to connect again.
#define K_DEF_DECODER_TIMEOUT_SECS  5       //seconds to wait for decoder-inactivity-timeout (frames are arriving from src, decoder is explicit-on but not producing output).
#define K_DEF_DECODER_RETRY_WAIT_SECS  5    //seconds to wait before trying to open device again.
#define K_DEF_ANIM_WAIT_SECS        10      //seconds to wait between streams position animations.
#define K_DEF_FRAMES_PER_SEC        25      //fps / screen-refreshs-per-second. Note: frames decoding is done as fast as posible, screen-refreshs draws the latest decoded frames to the screen.

#ifndef SOCKET
#   define SOCKET           int
#endif

#ifndef INVALID_SOCKET
#   define INVALID_SOCKET   (SOCKET)(~0)
#endif

#define CALL_IOCTL(D, M) \
    D = M; \
    if(D != 0){ \
        printf("" #M " returned %d at line %d.\n", D, __LINE__); \
    } else { \
        /*printf("" #M " success.\n");*/ \
    } \

//

struct STPlayer_;
struct STBuffer_;
struct STPlane_;

//STFbPos

typedef struct STFbPos_ {
    int x;   //left
    int y;    //top
} STFbPos;

//STFbSize

typedef struct STFbSize_ {
    int width;
    int height;
} STFbSize;

//STFbRect

typedef struct STFbRect_ {
    int x;   //left
    int y;    //top
    int width;
    int height;
} STFbRect;

//STThreadTask

typedef void (*ThreadTaskFunc)(void* param);

typedef struct STThreadTask_ {
    ThreadTaskFunc  func;
    void*           param;
} STThreadTask;

//STThread

typedef struct STThread_ {
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    pthread_t       thread;
    int             stopFlag;
    int             isRunning;
    //tasks
    struct {
        STThreadTask**  arr;
        int             use;
        int             sz;
    } tasks;
} STThread;

void Thread_init(STThread* obj);
void Thread_release(STThread* obj);
int Thread_start(STThread* obj);
int Thread_stopFlag(STThread* obj);
int Thread_waitForAll(STThread* obj);
int Thread_addTask(STThread* obj, ThreadTaskFunc func, void* param);

//STFbLayoutRect

typedef struct STFbLayoutRect_ {
    int             streamId;   //stream identifier (0 if black row)
    STFbRect        rect;       //location relative to row
} STFbLayoutRect;

void FbLayoutRect_init(STFbLayoutRect* obj);
void FbLayoutRect_release(STFbLayoutRect* obj);

//STFbLayoutRow

typedef struct STFbLayoutRow_ {
    int             iRow;
    int             yTop;       //natural yTop position in screen
    int             width;
    int             height;
    STFbLayoutRect* rects;      //rects are contained inside the row's height, rects with 'streamId == 0' are black rects.
    int             rectsUse;
    int             rectsSz;
} STFbLayoutRow;

void FbLayoutRow_init(STFbLayoutRow* obj);
void FbLayoutRow_release(STFbLayoutRow* obj);
int FbLayoutRow_add(STFbLayoutRow* obj, const int streamId, int x, int y, const int width, const int height); //will be 'x' ordered automatically
int FbLayoutRow_fillGaps(STFbLayoutRow* obj, const int widthMax);

//STFramebuff

typedef struct STFramebuffPtr_ {
    unsigned char*  ptr;
    int             ptrSz;
    int             isSynced;
    //
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    int             tasksPendCount; //tasks delegated to worker threads, waiting for results
} STFramebuffPtr;

void FramebuffPtr_init(STFramebuffPtr* obj);
void FramebuffPtr_release(STFramebuffPtr* obj);
    
typedef struct STFramebuff_ {
    //cfg
    struct {
        char*           device; // "/dev/fb0"
        int             animSecsWaits;
    } cfg;
    STFramebuffPtr      offscreen;  //offscreen buffer (malloc)
    STFramebuffPtr      screen;     //screen buffer (mmap)
    void*               blackLine;  //data for a balack full-line (bytesPerLn)
    int                 blackLineSz; //bytesPerLn
    //layout
    struct {
        //rows
        struct {
            STFbLayoutRow*  arr;
            int             use;
            int             sz;
            int             rectsCount;
        } rows;
        //layout total size
        int             width;   //ritgh side of all elements
        int             height;  //lower size of all elements
        //anim
        struct {
            unsigned long msWait;   //currently waiting
            int         iRowFirst;  //current top row
            int         yOffset;    //current offset
        } anim;
    } layout;
    //screen
    int                 fd;
    int                 pixFmt;
    int                 bitsPerPx;
    int                 bytesPerLn;
    int                 width;
    int                 height;
    struct fb_var_screeninfo vinfo; //variable info (FBIOGET_VSCREENINFO)
    struct fb_fix_screeninfo finfo; //fixed info (FBIOGET_FSCREENINFO)
} STFramebuff;

void Framebuff_init(STFramebuff* obj);
void Framebuff_release(STFramebuff* obj);
//
int Framebuff_open(STFramebuff* obj, const char* device, const int animSecsWaits);
int Framebuff_validateRect(STFramebuff* obj, STFbPos pPos, STFbPos* dstPos, STFbRect pSrcRect, STFbRect* dstRect);
int Framebuff_bitblit(STFramebuff* obj, STFramebuffPtr* dst, STFbPos dstPos, const struct STPlane_* srcPixs, STFbRect srcRect);
//
int Framebuff_layoutStart(STFramebuff* obj);    //resets 'isFound'
int Framebuff_layoutEnd(STFramebuff* obj);      //removes records without 'isFound' flag, and organizes in order
int Framebuff_layoutAdd(STFramebuff* obj, int streamId, const STFbSize size); //updates or add the stream, flags it as 'isFound'
//
int Framebuff_streamFindStreamId(STFramebuff* obj, int streamId);    //find this stream current location in scene
//
int Framebuff_animTick(STFramebuff* obj, int ms);
int Framebuff_drawToPtr(STFramebuff* obj, struct STPlayer_* plyr, STFramebuffPtr* dst);

//STPlane

typedef struct STPlane_ {
    int             dataPtrIsExternal; //0, the plane ptr was mallocated, 1, the plane ptr is owned externally and should not be freed
    unsigned char*  dataPtr;    //mmapped
    unsigned int    used;       //bytes populated (set by user on srcBuffers, set by the driver in dstBuffers)
    unsigned int    length;     //mmap param
    unsigned int    bytesPerLn;
    unsigned int    memOffset;  //mmap param
    int             fd;         //DMA exported buffer
} STPlane;

void Plane_init(STPlane* obj);
void Plane_release(STPlane* obj);
int Plane_clone(STPlane* obj, const STPlane* src);

//STBuffer

typedef struct STBuffer_ {
    int         index;
    int         isQueued;
    STPlane*    planes;
    int         planesSz;
    //dbg
#   ifdef K_DEBUG
    struct {
        int     indexPlusOne;   //to validate record
    } dbg;
#   endif
} STBuffer;

void Buffer_init(STBuffer* obj);
void Buffer_release(STBuffer* obj);
int Buffer_clone(STBuffer* obj, const STBuffer* src);

//STBuffers (output/source or capture/destination)

typedef struct STBuffers_ {
    char*       name;   //internal value, for dbg
    int         type;   //V4L2_BUF_TYPE_VIDEO_OUTPUT(_MPLANE), V4L2_BUF_TYPE_VIDEO_CAPTURE(_MPLANE)
    STBuffer*   arr;
    int         use;
    int         sz;
    int         enqueuedRequiredMin;    //minimun buffers required queued
    int         enqueuedCount;          //
    struct v4l2_format fm;
#   ifdef K_USE_MPLANE
    struct v4l2_pix_format_mplane* mp;  //multi-plane
#   else
    struct v4l2_pix_format* sp;         //single-plane
#   endif
    struct v4l2_buffer srchBuff;     //record for buffers requests
#   ifdef K_USE_MPLANE
    struct v4l2_plane* srchPlanes;   //record for buffers requests
#   endif
    int         pixelformat;
    int         width;
    int         height;
    STFbRect    composition;        //composition area (visibble area)
    //
    int         isExplicitON;       //start() with no stop() after
    int         isImplicitON;       //an event stopped this side of the device
    int         isLastDequeuedCloned; //determines if 'lastClone' should be used instead of 'lastDequeued'
    STBuffer*   lastDequeued;       //last dequeued buffer (if dst, then should contain the lastest decoded frame)
    STBuffer    lastDequeuedClone;  //temporal copy overwitten by 'Buffers_keepLastAsClone', used only if could not allocate an extra buffer after minimun.
    unsigned long msWithoutEnqueuing;     //ms without buffers enqueued
    unsigned long msWithoutDequeuing;     //ms without buffer dequeued
} STBuffers;

void Buffers_init(STBuffers* obj);
void Buffers_release(STBuffers* obj);

int Buffers_setNameAndType(STBuffers* obj, const char* name, const int type);
int Buffers_queryFmts(STBuffers* obj, int fd, int fmtSearch, int* dstFmtWasFound, const int print);
int Buffers_setFmt(STBuffers* obj, int fd, int fmt, int planesPerBuffer, int sizePerPlane, const int print);
int Buffers_getCompositionRect(STBuffers* obj, int fd, STFbRect* dstRect);
int Buffers_allocBuffs(STBuffers* obj, int fd, int ammount, const int print);
int Buffers_export(STBuffers* obj, int fd);
int Buffers_mmap(STBuffers* obj, int fd);
int Buffers_unmap(STBuffers* obj, int fd);
int Buffers_getUnqueued(STBuffers* obj, STBuffer** dstBuff, STBuffer* ignoreThis);        //get a buffer not enqueued yet
int Buffers_enqueueMinimun(STBuffers* obj, int fd, const int minimun);
int Buffers_enqueue(STBuffers* obj, int fd, STBuffer* srcBuff, const struct timeval* srcTimestamp);     //add to queue
int Buffers_dequeue(STBuffers* obj, int fd, STBuffer** dstBuff, struct timeval* dstTimestamp);     //remove from queue
int Buffers_start(STBuffers* obj, int fd);
int Buffers_stop(STBuffers* obj, int fd);
int Buffers_keepLastAsClone(STBuffers* obj, STBuffer* src);   //

//ENPlayerPollFdType

typedef enum ENPlayerPollFdType_ {
    ENPlayerPollFdType_Decoder = 0, //dec (decoder).fd
    ENPlayerPollFdType_SrcSocket,   //net.socket
    //
    ENPlayerPollFdType_Count
} ENPlayerPollFdType;

typedef void (*PlayerPollCallback)(void* userParam, struct STPlayer_* plyr, const ENPlayerPollFdType type, int revents);

//STPlayerPollFd

typedef struct STPlayerPollFd_ {
    ENPlayerPollFdType  type;
    int                 events;     //requested events
    void*               obj;
    PlayerPollCallback  callback;
    int                 autoremove; //orphan
} STPlayerPollFd;

//STVideoFrameState
//Allows to follow the timings of frames.

typedef struct STVideoFrameState_ {
    //NOTE: update 'VideoFrameState_clone()' if non-static-members are added.
    unsigned long       iSeq;           //sequence order (to  individually identify input and output frames)
    int                 isIndependent;  //IDR(-like) frame, capable of producing an output without referencing other frames.
    struct {
        //arrival
        struct {
            struct timeval  start;  //4-bytes header first seen
            struct timeval  end;    //next-frame 4-bytes header first seen
        } arrival;
        //proc
        struct {
            struct timeval  start;  //pushed for processing (decoder)
            struct timeval  end;    //returned by processor (decoder)
        } proc;
    } times;
    //NOTE: update 'VideoFrameState_clone()' if non-static-members are added.
} STVideoFrameState;

void VideoFrameState_init(STVideoFrameState* obj);
void VideoFrameState_release(STVideoFrameState* obj);
//
int VideoFrameState_reset(STVideoFrameState* obj);
int VideoFrameState_clone(STVideoFrameState* obj, const STVideoFrameState* src);
//
void VideoFrameState_iSeqToTimestamp(const unsigned long iSeq, struct timeval* dstTimestamp);
void VideoFrameState_timestampToSeqIdx(const struct timeval* srcTimestamp, unsigned long *dstSeq);

//STVideoFrameStates

typedef struct STVideoFrameStates_ {
    STVideoFrameState*  arr;    //oldest first
    int                 use;
    int                 sz;
} STVideoFrameStates;

void VideoFrameStates_init(STVideoFrameStates* obj);
void VideoFrameStates_release(STVideoFrameStates* obj);
//
int VideoFrameStates_getStateCloningAndRemoveOlder(STVideoFrameStates* obj, const unsigned long iSeq, STVideoFrameState* dstState, int* dstOlderRemovedCount);
int VideoFrameStates_addNewestCloning(STVideoFrameStates* obj, const STVideoFrameState* state);

//STVideoFrame
//In H264, an Access unit allways produces an output frame.
//IDR = Instantaneous Decoding Refresh

typedef struct STVideoFrame_ {
    STVideoFrameState   state;
    //buff
    struct {
        unsigned char*  ptr;
        int             use;
        int             sz;
    } buff;
} STVideoFrame;

void VideoFrame_init(STVideoFrame* obj);
void VideoFrame_release(STVideoFrame* obj);
//
int VideoFrame_reset(STVideoFrame* obj);           //state is reseted and buffer 'use' is set to zero
int VideoFrame_copy(STVideoFrame* obj, const void* data, const int dataSz);  //

//STVideoFrames

typedef struct STVideoFrames_ {
    unsigned long   iSeqNext;   //iSeq to set
    STVideoFrame**  arr;        //array of ptrs
    int             sz;
    int             use;
} STVideoFrames;

void VideoFrames_init(STVideoFrames* obj);
void VideoFrames_release(STVideoFrames* obj);
//
int VideoFrames_pullFrameForFill(STVideoFrames* obj, STVideoFrame** dst); //get from the right, reuse or creates a new one
int VideoFrames_peekFrameForRead(STVideoFrames* obj); //peek from the left
int VideoFrames_pullFrameForRead(STVideoFrames* obj, STVideoFrame** dst); //get from the left
int VideoFrames_pushFrameOwning(STVideoFrames* obj, STVideoFrame* src); //add for future pull (reuse)

//STStreamContext

typedef struct STStreamContext_ {
    int                 streamId;   //defined by the player
    //cfg
    struct {
        char*           device; //"/dev/video10"
        char*           server; //ip or dns
        unsigned int    port;   //port
        char*           path;   // "/folder/file.264"
        int             srcPixFmt; //V4L2_PIX_FMT_H264
        int             buffersAmmount;
        int             planesPerBuffer;
        int             sizePerPlane;
        int             dstPixFmt;
        int             connTimeoutSecs;
        int             decoderTimeoutSecs;
        int             animSecsWaits;
    } cfg;
    //dec (decoder)
    struct {
        int             fd;
        unsigned long   msOpen;         //time since open() returned this fd.
        unsigned long   msWithoutFeedFrame;  //to detect decoder-timeout
        unsigned long   msToReopen;  //
        int             isWaitingForIDRFrame;
        STBuffers       src;
        STBuffers       dst;
        //frames
        struct {
            STVideoFrameStates  fed; //frames currently in decoder
        } frames;
    } dec;
    //frames
    struct {
        STVideoFrame*   filling;    //currently filling frame
        int             fillingNalSz; //current (latest) NAL header and payload sz
        STVideoFrames   filled;     //filled with payload
        STVideoFrames   reusable;   //for reutilization
    } frames;
    //net
    struct {
        SOCKET          socket;         //async
        unsigned long   msWithoutSend;  //to detect connection-timeout
        unsigned long   msWithoutRecv;  //to detect connection-timeout
        unsigned long   msToReconnect;  //
        //buff
        struct {
            unsigned char*  buff;
            int         buffCsmd;   //left side consumed
            int         buffUse;    //right side producer
            int         buffSz;
        } buff;
        //req
        struct {
            char*       pay;
            int         payCsmd;    //sent
            int         payUse;
            int         paySz;
        } req;
        //resp
        struct {
            int         headerEndSeq;    //0 = [], 1 = ['\r'], 2 = ['\r', '\n'], 3 = ['\r', '\n', '\r']
            int         headerSz;        //
            int         headerEnded;     //"\r\n\r\n" found after connection
            //nal
            struct {
                int     zeroesSeqAccum;  //reading posible headers '0x00 0x00 0x00 0x01' (start of a NAL)
                int     startsCount;     //total NALs found (including the curently incomplete one)
            } nal;
        } resp;
    } net;
} STStreamContext;

void StreamContext_init(STStreamContext* ctx);
void StreamContext_release(STStreamContext* ctx);

//

int StreamContext_open(STStreamContext* ctx, struct STPlayer_* plyr, const char* device, const char* server, const unsigned int port, const char* resPath, int srcPixFmt /*V4L2_PIX_FMT_H264*/, int buffersAmmount, int planesPerBuffer, int sizePerPlane, int dstPixFmt /*V4L2_PIX_FMT_RGB565*/, const int connTimeoutSecs, const int decoderTimeoutSecs);
int StreamContext_close(STStreamContext* ctx, struct STPlayer_* plyr);
//
int StreamContext_concatHttpRequest(STStreamContext* ctx, char* dst, int dstSz);

void StreamContext_updatePollMask_(STStreamContext* ctx, struct STPlayer_* plyr);
int StreamContext_getMinBuffersForDst(STStreamContext* ctx, int* dstValue);
//
int StreamContext_initAndPrepareSrc(STStreamContext* ctx, int fd, const int buffersAmmount, const int print);
int StreamContext_initAndStartDst(STStreamContext* ctx, struct STPlayer_* plyr);
int StreamContext_stopAndUnmapBuffs(STStreamContext* ctx, STBuffers* buffs);
//
int StreamContext_eventsSubscribe(STStreamContext* ctx, int fd);
int StreamContext_eventsUnsubscribe(STStreamContext* ctx, int fd);

void StreamContext_tick(STStreamContext* ctx, struct STPlayer_* plyr, unsigned int ms);
int StreamContext_getPollEventsMask(STStreamContext* ctx);
void StreamContext_pollCallback(void* userParam, struct STPlayer_* plyr, const ENPlayerPollFdType type, int revents);

//STPrintedDef
//Used to avoid printing the same info multiple times

typedef struct STPrintedInfo_ {
    char*           device;     //device
    int             srcFmt;     //src format
    int             dstFmt;     //dst format
    struct timeval  last;       //last time printed
} STPrintedInfo;

void PrintedInfo_init(STPrintedInfo* obj);
void PrintedInfo_release(STPrintedInfo* obj);
//
int PrintedInfo_set(STPrintedInfo* obj, const char* device, const int srcFmt, const int dstFmt); //applies info
int PrintedInfo_touch(STPrintedInfo* obj);  //updated 'last' time

//STPlayer

typedef struct STPlayer_ {
    int                 stopFlag;
    int                 streamIdNext;   //to assign to streams
    unsigned long long  msRunning;
    //cfg
    struct {
        int             extraThreadsAmm;
        int             connTimeoutSecs;
        int             connWaitReconnSecs;
        int             decoderTimeoutSecs;
        int             decoderWaitRecopenSecs;
        int             animSecsWaits;
        int             screenRefreshPerSec;
        //dbg
        struct {
            int         simNetworkTimeout;  //(1/num) probability to trigger a simulated network timeout, for cleanup code test.
            int         simDecoderTimeout;  //(1/num) probability to trigger a simulated decoder timeout, for cleanup code test.
        } dbg;
    } cfg;
    //poll
    struct {
        STPlayerPollFd* fds;    //(max RLIMIT_NOFILE)
        struct pollfd*  fdsNat; //(max RLIMIT_NOFILE)
        int             fdsUse;
        int             fdsSz;
        int             autoremovesPend; //orphans
    } poll;
    //fbs (framebuffers, a.k.a screens)
    struct {
        STFramebuff**   arr;
        int             arrUse;
        int             arrSz;
    } fbs;
    //streams
    struct {
        STStreamContext** arr;
        int             arrUse;
        int             arrSz;
    } streams;
    //threads
    struct {
        STThread*   arr;
        int         use;
        int         sz;
    } threads;
    //stats
    struct {
        pthread_mutex_t mutex;
        //curSec (reseted each second)
        struct {
            unsigned long long drawMsMin;   //ms-min
            unsigned long long drawMsMax;   //ms-max
            unsigned long long drawMsSum;   //ms-sum
            unsigned long long drawCount;   //times
        } curSec;
    } stats;
    //printf
    struct {
        STPrintedInfo** arr;
        int             use;
        int             sz;
    } prints;
} STPlayer;

void Player_init(STPlayer* obj);
void Player_release(STPlayer* obj);

//prints
STPrintedInfo* Player_getPrint(STPlayer* obj, const char* device, const int srcFmt, const int dstFmt);
STPrintedInfo* Player_getPrintIfNotRecent(STPlayer* obj, const char* device, const int srcFmt, const int dstFmt, const int secsRecentMax);

//threads (optionals)
int Player_createExtraThreads(STPlayer* obj, int extraThreadsAmm);

//poll
int Player_pollAdd(STPlayer* obj, const ENPlayerPollFdType type, PlayerPollCallback callback, void* objPtr, const int fd, const int events);
int Player_pollUpdate(STPlayer* obj, const ENPlayerPollFdType type, const void* objPtr, const int fd, const int events, int* dstEventsBefore);
int Player_pollAutoRemove(STPlayer* obj, const ENPlayerPollFdType type, const void* objPtr, const int fd); //flag to be removed (safe inside poll-events)
//int Player_pollRemove_(STPlayer* obj, const ENPlayerPollFdType type, const void* objPtr, const int fd);  //remove inmediatly (unsafe inside poll-events)

//fbs
int Player_fbAdd(STPlayer* obj, const char* device, const int animSecsWaits);
int Player_fbRemove(STPlayer* obj, STFramebuff* stream);

//streams
int Player_streamAdd(STPlayer* obj, const char* device, const char* server, const unsigned int port, const char* resPath, const int connTimeoutSecs, const int decoderTimeoutSecs);
int Player_streamRemove(STPlayer* obj, STStreamContext* stream);

//organize
//Assigns fb and screen-area to each stream. Should be called on each stream resize event.
int Player_organize(STPlayer* obj);
int Player_tick(STPlayer* obj, int ms);
//

int v4lDevice_queryCaps(int fd, const int print);
int v4lDevice_queryControls(int fd, const int print);
int v4lDevice_controlAnalyze(int fd, struct v4l2_queryctrl* ctrl, const int print);
//
long msBetweenTimevals(struct timeval* base, struct timeval* next);
long msBetweenTimespecs(struct timespec* base, struct timespec* next);

//----------
//-- main --
//----------

int __stopInterrupt = 0;

typedef enum ENSignalAction_ {
    ENSignalAction_Ignore = 0,
    ENSignalAction_GracefullExit,
    ENSignalAction_Count
} ENSignalAction;

typedef struct STSignalDef_ {
    int                sig;
    const char*        sigName;
    ENSignalAction    action;
} STSignalDef;

static STSignalDef _signalsDefs[] = {
    //sockets SIGPIPE signals (for unix-like systems)
    { SIGPIPE, "SIGPIPE", ENSignalAction_Ignore}, //Ignore
    //termination signals: https://www.gnu.org/software/libc/manual/html_node/Termination-Signals.html
    { SIGTERM, "SIGTERM", ENSignalAction_GracefullExit },
    { SIGINT, "SIGINT", ENSignalAction_GracefullExit },
    { SIGQUIT, "SIGQUIT", ENSignalAction_GracefullExit },
    { SIGKILL, "SIGKILL", ENSignalAction_GracefullExit },
    { SIGHUP, "SIGHUP", ENSignalAction_GracefullExit },
};

void intHandler(int sig){
    //Note: interruptions are called without considerations
    //of mutexes and threads states. Avoid non interrupt-safe methods calls.
    int i; const int count = (sizeof(_signalsDefs) / sizeof(_signalsDefs[0]));
    for(i = 0; i < count; i++){
        const STSignalDef* def = &_signalsDefs[i];
        if(sig == def->sig){
            if(def->action == ENSignalAction_GracefullExit){
                __stopInterrupt = 1;
            }
            break;
        }
    }
}

void printHelp(void){
    printf("Params:\n");
    printf("\n");
    printf("-h, --help                prints this text.\n");
    printf("-dcb, --disableCursorBlinking, writes '0' at '/sys/class/graphics/fbcon/cursor_blink'.\n");
    printf("-t, --extraThreads num    extra threads for rendering (default: %d).\n", K_DEF_THREADS_EXTRA_AMM);
    printf("-cto, --connTimeout num   seconds without conn activity to restart connection (default: %ds).\n", K_DEF_CONN_TIMEOUT_SECS);
    printf("-crc, --connWaitReconnect num, seconds to wait before reconnect (default: %ds).\n", K_DEF_CONN_RETRY_WAIT_SECS);
    printf("-dto, --decTimeout num    seconds without decoder output to restart decoder (default: %ds).\n", K_DEF_DECODER_TIMEOUT_SECS);
    printf("-dro, --decWaitReopen num, seconds to wait before reopen decoder device (default: %ds).\n", K_DEF_DECODER_RETRY_WAIT_SECS);
    printf("-aw, --animWait num       seconds between animation steps (default: %ds).\n", K_DEF_ANIM_WAIT_SECS);
    printf("-fps, --framesPerSec num  screen frames/refresh per second (default: %d).\n", K_DEF_FRAMES_PER_SEC);
    printf("\n");
    printf("-fb, --frameBuffer path   adds a framebuffer device (like '/dev/fb0').\n");
    printf("\n");
    printf("-dec, --decoder path      set the path to decoder device (like '/dev/video0') for next streams.\n");
    printf("-srv, --server name/ip    set the name/ip to server for next streams.\n");
    printf("-p, --port num            set the port number for next streams.\n");
    printf("-s, --stream path         adds a stream source (like '/http/relative/path/file.h.264').\n");
    printf("\n");
    printf("DEBUG OPTIONS:\n");
    printf("--secsRunAndExit num      seconds after starting to automatically activate stop-flag and exit, for debug and test.\n");
    printf("--secsSleepBeforeExit num seconds to sleep before exiting the main() funcion, for memory leak detection.\n");
    printf("--simNetworkTimeout num   (1/num) probability to trigger a simulated network timeout, for cleanup code test.\n");
    printf("--simDecoderTimeout num   (1/num) probability to trigger a simulated decoder timeout, for cleanup code test.\n");
    printf("\n");
}
    
int main(int argc, char* argv[]){
    int r = -1; int helpPrinted = 0, errorFatal = 0;
    int secsRunAndExit = 0, secsSleepBeforeExit = 0;
    STPlayer* p = (STPlayer*)malloc(sizeof(STPlayer));
    //random initialization
    srand(time(NULL));
    //
    Player_init(p);
    //defaults
    {
        p->cfg.extraThreadsAmm          = K_DEF_THREADS_EXTRA_AMM;
        p->cfg.connTimeoutSecs          = K_DEF_CONN_TIMEOUT_SECS;
        p->cfg.connWaitReconnSecs       = K_DEF_CONN_RETRY_WAIT_SECS;
        p->cfg.decoderTimeoutSecs       = K_DEF_DECODER_TIMEOUT_SECS;
        p->cfg.decoderWaitRecopenSecs   = K_DEF_DECODER_RETRY_WAIT_SECS;
        p->cfg.animSecsWaits            = K_DEF_ANIM_WAIT_SECS;
        p->cfg.screenRefreshPerSec      = K_DEF_FRAMES_PER_SEC;
    }
    //Apply signal handlers.
    //Ignore SIGPIPE at process level (for unix-like systems)
    {
        int i; const int count = (sizeof(_signalsDefs) / sizeof(_signalsDefs[0]));
        for(i = 0; i < count; i++){
            const STSignalDef* def = &_signalsDefs[i];
            if(def->action == ENSignalAction_Ignore){
                struct sigaction act;
                act.sa_handler    = SIG_IGN;
                act.sa_flags    = 0;
                sigemptyset(&act.sa_mask);
                sigaction(def->sig, &act, NULL);
            } else if(def->action == ENSignalAction_GracefullExit){
                struct sigaction act;
                act.sa_handler    = intHandler;
                act.sa_flags    = 0;
                sigemptyset(&act.sa_mask);
                sigaction(def->sig, &act, NULL);
            }
        }
    }
    //parse params
    {
        const char* decoder = NULL;
        const char* server = NULL;
        int port = 0;
        int i; for(i = 0; i < argc; i++){
            const char* arg = argv[i];
            if(strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0){
                if(!helpPrinted){
                    printHelp();
                    helpPrinted = 1;
                }
            } else if(strcmp(arg, "-dcb") == 0 || strcmp(arg, "--disableCursorBlinking") == 0){
                int fd = open("/sys/class/graphics/fbcon/cursor_blink", O_WRONLY);
                if(fd < 0){
                    printf("ERROR, clould not open '/sys/class/graphics/fbcon/cursor_blink'.\n");
                } else {
                    if(write(fd, "0", 1) != 1){
                        printf("ERROR, clould not write '/sys/class/graphics/fbcon/cursor_blink'.\n");
                    } else {
                        printf("Param, cursor blink disabled '/sys/class/graphics/fbcon/cursor_blink'.\n");
                    }
                    close(fd);
                    fd = -1;
                }
            } else if(strcmp(arg, "-t") == 0 || strcmp(arg, "--extraThreads") == 0){
                if((i + 1) < argc){
                    const char* val = argv[i + 1];
                    char* endPtr = NULL;
                    const long int v = strtol(val, &endPtr, 0);
                    if(*endPtr != '\0'){
                        printf("Param '--extraThreads' value is not valid: '%s'\n", val);
                    } else {
                        p->cfg.extraThreadsAmm = v;
                        printf("Param '--extraThreads' value set: '%d'\n", v);
                    }
                    i++;
                }
            } else if(strcmp(arg, "-cto") == 0 || strcmp(arg, "--connTimeout") == 0){
                if((i + 1) < argc){
                    const char* val = argv[i + 1];
                    char* endPtr = NULL;
                    const long int v = strtol(val, &endPtr, 0);
                    if(*endPtr != '\0'){
                        printf("Param '--connTimeout' value is not valid: '%s'\n", val);
                    } else {
                        p->cfg.connTimeoutSecs = v;
                        printf("Param '--connTimeout' value set: '%d'\n", v);
                    }
                    i++;
                }
            } else if(strcmp(arg, "-crc") == 0 || strcmp(arg, "--connWaitReconnect") == 0){
                if((i + 1) < argc){
                    const char* val = argv[i + 1];
                    char* endPtr = NULL;
                    const long int v = strtol(val, &endPtr, 0);
                    if(*endPtr != '\0' || v <= 0){ //0 is not allowed
                        printf("Param '--connWaitReconnect' value is not valid: '%s'\n", val);
                    } else {
                        p->cfg.connWaitReconnSecs = v;
                        printf("Param '--connWaitReconnect' value set: '%d'\n", v);
                    }
                    i++;
                }
            } else if(strcmp(arg, "-dto") == 0 || strcmp(arg, "--decTimeout") == 0){
                if((i + 1) < argc){
                    const char* val = argv[i + 1];
                    char* endPtr = NULL;
                    const long int v = strtol(val, &endPtr, 0);
                    if(*endPtr != '\0'){
                        printf("Param '--decTimeout' value is not valid: '%s'\n", val);
                    } else {
                        p->cfg.decoderTimeoutSecs = v;
                        printf("Param '--decTimeout' value set: '%d'\n", v);
                    }
                    i++;
                }
            } else if(strcmp(arg, "-dro") == 0 || strcmp(arg, "--decWaitReopen") == 0){
                if((i + 1) < argc){
                    const char* val = argv[i + 1];
                    char* endPtr = NULL;
                    const long int v = strtol(val, &endPtr, 0);
                    if(*endPtr != '\0' || v <= 0){ //0 is not allowed
                        printf("Param '--decWaitReopen' value is not valid: '%s'\n", val);
                    } else {
                        p->cfg.decoderWaitRecopenSecs = v;
                        printf("Param '--decWaitReopen' value set: '%d'\n", v);
                    }
                    i++;
                }
            } else if(strcmp(arg, "-aw") == 0 || strcmp(arg, "--animWait") == 0){
                if((i + 1) < argc){
                    const char* val = argv[i + 1];
                    char* endPtr = NULL;
                    const long int v = strtol(val, &endPtr, 0);
                    if(*endPtr != '\0'){
                        printf("Param '--animWait' value is not valid: '%s'\n", val);
                    } else {
                        p->cfg.animSecsWaits = v;
                        printf("Param '--animWait' value set: '%d'\n", v);
                    }
                    i++;
                }
            } else if(strcmp(arg, "-fps") == 0 || strcmp(arg, "--framesPerSec") == 0){
                if((i + 1) < argc){
                    const char* val = argv[i + 1];
                    char* endPtr = NULL;
                    const long int v = strtol(val, &endPtr, 0);
                    if(*endPtr != '\0'){
                        printf("Param '--framesPerSec' value is not valid: '%s'\n", val);
                    } else {
                        p->cfg.screenRefreshPerSec = v;
                        printf("Param '--framesPerSec' value set: '%d'\n", v);
                    }
                    i++;
                }
            } else if(strcmp(arg, "-fb") == 0 || strcmp(arg, "--frameBuffer") == 0){
                if((i + 1) < argc){
                    const char* val = argv[i + 1];
                    if(0 != Player_fbAdd(p, val, p->cfg.animSecsWaits)){
                        printf("ERROR, main, could not add fb.\n");
                        errorFatal = 1;
                    } else {
                        printf("Main, fb added: '%s'.\n", val);
                    }
                    i++;
                }
            } else if(strcmp(arg, "-dec") == 0 || strcmp(arg, "--decoder") == 0){
                if((i + 1) < argc){
                    decoder = argv[i + 1];
                    printf("Param '--decoder' value set: '%s'\n", argv[i + 1]);
                    i++;
                }
            } else if(strcmp(arg, "-srv") == 0 || strcmp(arg, "--server") == 0){
                if((i + 1) < argc){
                    server = argv[i + 1];
                    printf("Param '--server' value set: '%s'\n", argv[i + 1]);
                    i++;
                }
            } else if(strcmp(arg, "-p") == 0 || strcmp(arg, "--port") == 0){
                if((i + 1) < argc){
                    const char* val = argv[i + 1];
                    char* endPtr = NULL;
                    const long int v = strtol(val, &endPtr, 0);
                    if(*endPtr != '\0'){
                        printf("Param '--port' value is not valid: '%s'\n", val);
                    } else {
                        port = v;
                        printf("Param '--port' value set: '%d'\n", v);
                    }
                    i++;
                }
            } else if(strcmp(arg, "-s") == 0 || strcmp(arg, "--stream") == 0){
                if((i + 1) < argc){
                    const char* val = argv[i + 1];
                    if(decoder == NULL){
                        printf("ERROR, param '--stream' missing previous param: '--decoder'.\n");
                        errorFatal = 1;
                    } else if(server == NULL){
                        printf("ERROR, param '--stream' missing previous param: '--server'.\n");
                        errorFatal = 1;
                    } else if(port <= 0){
                        printf("ERROR, param '--stream' missing previous param: '--port'.\n");
                        errorFatal = 1;
                    } else if(0 != Player_streamAdd(p, decoder, server, port, val, p->cfg.connTimeoutSecs, p->cfg.decoderTimeoutSecs)){
                        printf("ERROR, main, could not add stream: '%s'.\n", val);
                        errorFatal = 1;
                    } else {
                        printf("Main, stream added: '%s'.\n", val);
                    }
                    i++;
                }
            } else if(strcmp(arg, "--secsRunAndExit") == 0){
                if((i + 1) < argc){
                    const char* val = argv[i + 1];
                    char* endPtr = NULL;
                    const long int v = strtol(val, &endPtr, 0);
                    if(*endPtr != '\0' || v < 0){
                        printf("Param '--secsRunAndExit' value is not valid: '%s'\n", val);
                    } else {
                        secsRunAndExit = v;
                        printf("Param '--secsRunAndExit' value set: '%d'\n", v);
                    }
                    i++;
                }
            } else if(strcmp(arg, "--secsSleepBeforeExit") == 0){
                if((i + 1) < argc){
                    const char* val = argv[i + 1];
                    char* endPtr = NULL;
                    const long int v = strtol(val, &endPtr, 0);
                    if(*endPtr != '\0' || v < 0){
                        printf("Param '--secsSleepBeforeExit' value is not valid: '%s'\n", val);
                    } else {
                        secsSleepBeforeExit = v;
                        printf("Param '--secsSleepBeforeExit' value set: '%d'\n", v);
                    }
                    i++;
                }
            } else if(strcmp(arg, "--simNetworkTimeout") == 0){
                if((i + 1) < argc){
                    const char* val = argv[i + 1];
                    char* endPtr = NULL;
                    const long int v = strtol(val, &endPtr, 0);
                    if(*endPtr != '\0' || v < 0){
                        printf("Param '--simNetworkTimeout' value is not valid: '%s'\n", val);
                    } else {
                        p->cfg.dbg.simNetworkTimeout = v;
                        printf("Param '--simNetworkTimeout' value set: '%d'\n", v);
                    }
                    i++;
                }
            } else if(strcmp(arg, "--simDecoderTimeout") == 0){
                if((i + 1) < argc){
                    const char* val = argv[i + 1];
                    char* endPtr = NULL;
                    const long int v = strtol(val, &endPtr, 0);
                    if(*endPtr != '\0' || v < 0){
                        printf("Param '--simDecoderTimeout' value is not valid: '%s'\n", val);
                    } else {
                        p->cfg.dbg.simDecoderTimeout = v;
                        printf("Param '--simDecoderTimeout' value set: '%d'\n", v);
                    }
                    i++;
                }
            }
        }
    }
    //execute
    if(!errorFatal){
        if(p->streams.arrUse <= 0){
            printf("Main, no streams loaded.\n");
            if(!helpPrinted){
                printHelp();
                helpPrinted = 1;
            }
        } else {
            unsigned long secsRunnning = 0;
            unsigned int animMsPerFrame = (1000 / (p->cfg.screenRefreshPerSec <= 0 ? 1 : p->cfg.screenRefreshPerSec)), animMsAccum = 0;
            struct timeval timePrev, timeCur;
            struct timeval animPrev, animCur;
            gettimeofday(&timePrev, NULL);
            gettimeofday(&animPrev, NULL);
            timeCur = timePrev;
            animCur = animPrev;
            //
            while(!p->stopFlag && !__stopInterrupt){
                //autoremove
                if(p->poll.autoremovesPend > 0){
                    int i; for(i = (int)p->poll.fdsUse - 1; i >= 0; i--){
                        const STPlayerPollFd* fdd = &p->poll.fds[i];
                        if(fdd->autoremove){
                            const struct pollfd* fd = &p->poll.fdsNat[i];
                            //remove
                            p->poll.fdsUse--;
                            for(;i < p->poll.fdsUse; i++){
                                p->poll.fds[i] = p->poll.fds[i + 1];
                                p->poll.fdsNat[i] = p->poll.fdsNat[i + 1];
                            }
                            //
                            printf("Main, fd-poll-autoremoved.\n");
                        }
                    }
                    //reset
                    p->poll.autoremovesPend = 0;
                }
                //poll
                if(p->poll.fdsUse <= 0){
                    //just sleep
                    int mSecs = (animMsPerFrame / 2);
                    usleep((useconds_t)(mSecs <= 0 ? 1 : mSecs) * 1000);
                } else {
                    const int msTimeout = 40;
                    const int rr = poll(p->poll.fdsNat, p->poll.fdsUse, msTimeout);
                    if(rr > 0){
                        int i, fndCount = 0;
                        for(i = 0; i < p->poll.fdsUse && fndCount < rr; i++){
                            const STPlayerPollFd* fdd = &p->poll.fds[i];
                            const struct pollfd* fd = &p->poll.fdsNat[i];
                            if(fd->revents != 0){
                                //print (tmp)
                                if(fd->revents != POLLERR){
                                    switch (fdd->type) {
                                        case ENPlayerPollFdType_Decoder:
                                            //printf("Main, decoder poll: %s%s%s%s%s%s.\n", (fd->revents & POLLOUT ? " POLLOUT" : ""), (fd->revents & POLLWRNORM ? " POLLWRNORM" : ""), (fd->revents & POLLIN ? " POLLIN" : ""), (fd->revents & POLLRDNORM ? " POLLRDNORM" : ""), (fd->revents & POLLERR ? " POLLERR" : ""), (fd->revents & POLLPRI ? " POLLPRI" : ""));
                                            break;
                                        case ENPlayerPollFdType_SrcSocket:
                                            //printf("Main, nret-socket poll: %s%s%s%s%s%s.\n", (fd->revents & POLLOUT ? " POLLOUT" : ""), (fd->revents & POLLWRNORM ? " POLLWRNORM" : ""), (fd->revents & POLLIN ? " POLLIN" : ""), (fd->revents & POLLRDNORM ? " POLLRDNORM" : ""), (fd->revents & POLLERR ? " POLLERR" : ""), (fd->revents & POLLPRI ? " POLLPRI" : ""));
                                            break;
                                        default:
                                            printf("Main, unknow-type poll: %s%s%s%s%s%s.\n", (fd->revents & POLLOUT ? " POLLOUT" : ""), (fd->revents & POLLWRNORM ? " POLLWRNORM" : ""), (fd->revents & POLLIN ? " POLLIN" : ""), (fd->revents & POLLRDNORM ? " POLLRDNORM" : ""), (fd->revents & POLLERR ? " POLLERR" : ""), (fd->revents & POLLPRI ? " POLLPRI" : ""));
                                            break;
                                    }
                                }
                                //call
                                if(fdd->callback != NULL){
                                    (fdd->callback)(fdd->obj, p, fdd->type, fd->revents);
                                }
                                fndCount++;
                            }
                        }
                    }
                }
                //anim
                {
                    gettimeofday(&animCur, NULL);
                    long ms = msBetweenTimevals(&animPrev, &animCur);
                    if(ms > 0 && (ms + animMsAccum) >= animMsPerFrame){
                        if(0 != Player_tick(p, animMsPerFrame)){
                            printf("Main, anim-tick fail.\n");
                        }
                        animMsAccum = (ms + animMsAccum) % animMsPerFrame;
                        animPrev = animCur;
                    }
                }
                //time passed
                gettimeofday(&timeCur, NULL);
                long ms = msBetweenTimevals(&timePrev, &timeCur);
                if(ms >= 1000){
                    //stats
                    {
                        pthread_mutex_lock(&p->stats.mutex);
                        {
                            if(p->stats.curSec.drawCount <= 0){
                                printf("Main, sec: drawn(%u).\n", p->stats.curSec.drawCount);
                            } else if(p->stats.curSec.drawMsMin <= 0){
                                printf("Main, sec: drawn(%u, %u/%u/%u ms).\n", p->stats.curSec.drawCount, p->stats.curSec.drawMsMin, p->stats.curSec.drawMsSum / p->stats.curSec.drawCount, p->stats.curSec.drawMsMax);
                            } else {
                                printf("Main, sec: drawn(%u, %u/%u/%u ms, %u/%u/%u fps max).\n", p->stats.curSec.drawCount, p->stats.curSec.drawMsMin, p->stats.curSec.drawMsSum / p->stats.curSec.drawCount, p->stats.curSec.drawMsMax, 1000ULL / p->stats.curSec.drawMsMax, 1000ULL / (p->stats.curSec.drawMsSum / p->stats.curSec.drawCount), 1000ULL / p->stats.curSec.drawMsMin);
                            }
                            //reset
                            memset(&p->stats.curSec, 0, sizeof(p->stats.curSec));
                        }
                        pthread_mutex_unlock(&p->stats.mutex);
                    }
                    timePrev = timeCur;
                    secsRunnning++;
                    //
                    if(secsRunAndExit > 0 && secsRunAndExit == secsRunnning){
                        printf("Main, stop-interrupt auto-activated after %u secs running.\n", secsRunnning);
                        __stopInterrupt = 1;
                    }
                }
            }
        }
    }
    if(__stopInterrupt){
        printf("Main, ending (stop-interrupted)...\n");
    } else {
        printf("Main, ending...\n");
    }
    if(p != NULL){
        Player_release(p);
        free(p);
        p = NULL;
    }
    if(__stopInterrupt){
        printf("Main, ended (stop-interrupted).\n");
    } else {
        printf("Main, ended.\n");
    }
    //
    if(secsSleepBeforeExit > 0){
        unsigned long s = 0;
        while(s < secsSleepBeforeExit){
            printf("Main, waiting %u/%u secs before exiting main().\n", (s + 1), secsSleepBeforeExit);
            sleep(1);
            s++;
        }
    }
    //
    return r;
}

//--------------
//-- STPlayer --
//--------------

void Player_init(STPlayer* obj){
    memset(obj, 0, sizeof(*obj));
    //
    //stats
    {
        pthread_mutex_init(&obj->stats.mutex, NULL);
    }
}

void Player_release(STPlayer* obj){
    //threads
    {
        if(obj->threads.arr != NULL){
            int i;
            //stop flags
            for(i = 0; i < obj->threads.use; i++){
                STThread* t = &obj->threads.arr[i];
                Thread_stopFlag(t);
            }
            //wait
            for(i = 0; i < obj->threads.use; i++){
                STThread* t = &obj->threads.arr[i];
                Thread_stopFlag(t);
                Thread_waitForAll(t);
            }
            free(obj->threads.arr);
            obj->threads.arr = NULL;
        }
        obj->threads.use = 0;
        obj->threads.sz = 0;
    }
    //fbs
    {
        if(obj->fbs.arr != NULL){
            int i; for(i = 0; i < obj->fbs.arrUse; i++){
                Framebuff_release(obj->fbs.arr[i]);
                free(obj->fbs.arr[i]);
                obj->fbs.arr[i] = NULL;
            }
            free(obj->fbs.arr);
            obj->fbs.arr = NULL;
        }
        obj->fbs.arrUse = 0;
        obj->fbs.arrSz = 0;
    }
    //streams
    {
        if(obj->streams.arr != NULL){
            int i; for(i = 0; i < obj->streams.arrUse; i++){
                StreamContext_release(obj->streams.arr[i]);
                free(obj->streams.arr[i]);
                obj->streams.arr[i] = NULL;
            }
            free(obj->streams.arr);
            obj->streams.arr = NULL;
        }
        obj->streams.arrUse = 0;
        obj->streams.arrSz = 0;
    }
    //poll
    {
        if(obj->poll.fds != NULL){
            free(obj->poll.fds);
            obj->poll.fds = NULL;
        }
        if(obj->poll.fdsNat != NULL){
            free(obj->poll.fdsNat);
            obj->poll.fdsNat = NULL;
        }
        obj->poll.fdsUse = 0;
        obj->poll.fdsSz = 0;
    }
    //stats
    {
        pthread_mutex_destroy(&obj->stats.mutex);
    }
    //prints
    {
        if(obj->prints.arr != NULL){
            int i; for(i = 0; i < obj->prints.use; i++){
                STPrintedInfo* p = obj->prints.arr[i];
                PrintedInfo_release(p);
                free(p);
            }
            free(obj->prints.arr);
        }
        obj->prints.use = 0;
        obj->prints.sz = 0;
    }
}

//prints

STPrintedInfo* Player_getPrint(STPlayer* obj, const char* device, const int srcFmt, const int dstFmt){
    STPrintedInfo* r = NULL;
    //search
    if(obj->prints.arr != NULL){
        int i; for(i = 0; i < obj->prints.use; i++){
            STPrintedInfo* p = obj->prints.arr[i];
            if(p->srcFmt == srcFmt && p->dstFmt == dstFmt && strcmp(p->device, device) == 0){
                r = p;
                break;
            }
        }
    }
    //create
    if(r == NULL){
        //resize array
        while(obj->prints.use >= obj->prints.sz){
            const int szN = (obj->prints.use + 16);
            STPrintedInfo** arrN = malloc(sizeof(STPrintedInfo*) * szN);
            if(arrN == NULL){
                break;
            } else {
                if(obj->prints.arr != NULL){
                    if(obj->prints.use > 0){
                        memcpy(arrN, obj->prints.arr, sizeof(obj->prints.arr[0]) * obj->prints.use);
                    }
                    free(obj->prints.arr);
                }
                obj->prints.arr = arrN;
                obj->prints.sz = szN;
            }
        }
        //add
        if(obj->prints.use < obj->prints.sz){
            STPrintedInfo* p = (STPrintedInfo*)malloc(sizeof(STPrintedInfo));
            PrintedInfo_init(p);
            PrintedInfo_set(p, device, srcFmt, dstFmt);
            obj->prints.arr[obj->prints.use++] = p;
            r = p;
        }
    }
    return r;
}

STPrintedInfo* Player_getPrintIfNotRecent(STPlayer* obj, const char* device, const int srcFmt, const int dstFmt, const int secsRecentMax){
    STPrintedInfo* r = Player_getPrint(obj, device, srcFmt, dstFmt);
    if(r->last.tv_sec > 0){
        //eval
        struct timeval now; long ms = 0;
        gettimeofday(&now, NULL);
        ms = msBetweenTimevals(&r->last, &now);
        if((ms / 1000) <= secsRecentMax){
            //printf("Player, print('%s' : %d : %d): %d secs ago (IGNORING).\n", device, srcFmt, dstFmt, (ms / 1000));
            r = NULL;
        } else {
            //printf("Player, print('%s' : %d : %d): %d secs ago (RETURNING).\n", device, srcFmt, dstFmt, (ms / 1000));
        }
    } else {
        //printf("Player, print('%s' : %d : %d): n-secs ago (NEVER TOUCHED).\n", device, srcFmt, dstFmt);
    }
    return r;
}

//threads (optionals)

int Player_createExtraThreads(STPlayer* obj, int extraThreadsAmm){
    //remove previous
    {
        if(obj->threads.arr != NULL){
            int i;
            //stop flags
            for(i = 0; i < obj->threads.use; i++){
                STThread* t = &obj->threads.arr[i];
                Thread_stopFlag(t);
            }
            //wait
            for(i = 0; i < obj->threads.use; i++){
                STThread* t = &obj->threads.arr[i];
                Thread_stopFlag(t);
                Thread_waitForAll(t);
            }
            free(obj->threads.arr);
            obj->threads.arr = NULL;
        }
        obj->threads.use = 0;
        obj->threads.sz = 0;
    }
    //create new
    if(extraThreadsAmm > 0){
        obj->threads.arr = malloc(sizeof(STThread) * extraThreadsAmm);
        if(obj->threads.arr != NULL){
            obj->threads.sz = extraThreadsAmm;
            obj->threads.use = 0;
            //start
            {
                int i; for(i = 0; i < extraThreadsAmm; i++){
                    STThread* t = &obj->threads.arr[i];
                    Thread_init(t);
                    if(0 != Thread_start(t)){
                        printf("Player_createExtraThreads, Thread_start failed.\n");
                        Thread_release(t);
                        break;
                    } else {
                        obj->threads.use++;
                    }
                }
            }
        }
    }
    return (obj->threads.use == extraThreadsAmm ? 0 : -1);
}


//fds

int Player_pollAdd(STPlayer* obj, const ENPlayerPollFdType type, PlayerPollCallback callback, void* objPtr, const int fd, const int events){
    int r = -1;
    //resize arr
    while(obj->poll.fdsUse >= obj->poll.fdsSz){
        STPlayerPollFd* fdsN = NULL;
        struct pollfd* fdsNatN = NULL;
        //
        fdsN = malloc(sizeof(STPlayerPollFd) * (obj->poll.fdsSz + 1));
        fdsNatN = malloc(sizeof(struct pollfd) * (obj->poll.fdsSz + 1));
        if(fdsN == NULL || fdsNatN == NULL){
            if(fdsN != NULL) free(fdsN); fdsN = NULL;
            if(fdsNatN != NULL) free(fdsNatN); fdsNatN = NULL;
            break;
        } else {
            obj->poll.fdsSz++;
            if(obj->poll.fds != NULL){
                if(obj->poll.fdsUse > 0){
                    memcpy(fdsN, obj->poll.fds, sizeof(STPlayerPollFd) * obj->poll.fdsUse);
                    memcpy(fdsNatN, obj->poll.fdsNat, sizeof(struct pollfd) * obj->poll.fdsUse);
                }
                free(obj->poll.fds);
                free(obj->poll.fdsNat);
            }
            obj->poll.fds = fdsN;
            obj->poll.fdsNat = fdsNatN;
        }
    }
    //add record
    if((obj->poll.fdsUse + 1) <= obj->poll.fdsSz){
        STPlayerPollFd* fdsN = &obj->poll.fds[obj->poll.fdsUse];
        struct pollfd* fdsNatN = &obj->poll.fdsNat[obj->poll.fdsUse];
        memset(fdsN, 0, sizeof(*fdsN));
        memset(fdsNatN, 0, sizeof(*fdsNatN));
        //
        fdsN->type  = type;
        fdsN->obj   = objPtr;
        fdsN->events = events;
        fdsN->callback = callback;
        //write(POLLOUT) read(POLLIN)
        //output-src(POLLOUT, POLLWRNORM) capture-dst(POLLIN, POLLRDNORM), src-dst-not-running(POLLERR), events(POLLPRI)
        fdsNatN->events = events;
        fdsNatN->fd     = fd;
        //
        obj->poll.fdsUse++;
        r = 0;
    }
    //
    return r;
}

int Player_pollUpdate(STPlayer* obj, const ENPlayerPollFdType type, const void* objPtr, const int fd, const int events, int* dstEventsBefore){
    int r = -1;
    //search
    int i; for(i = 0; i < obj->poll.fdsUse; i++){
        STPlayerPollFd* fdsN = &obj->poll.fds[i];
        struct pollfd* fdsNatN = &obj->poll.fdsNat[i];
        if(fdsN->type == type && fdsN->obj == objPtr && fdsNatN->fd == fd){
            if(dstEventsBefore != NULL){
                *dstEventsBefore = fdsN->events;
            }
            fdsN->events = events;
            fdsNatN->events = events;
            r = 0;
            break;
        }
    }
    return r;
}

//flag to be removed (safe inside poll-events)
int Player_pollAutoRemove(STPlayer* obj, const ENPlayerPollFdType type, const void* objPtr, const int fd){
    int r = -1;
    //search
    int i; for(i = 0; i < obj->poll.fdsUse; i++){
        STPlayerPollFd* fdsN = &obj->poll.fds[i];
        struct pollfd* fdsNatN = &obj->poll.fdsNat[i];
        if(fdsN->type == type && fdsN->obj == objPtr && fdsNatN->fd == fd){
            fdsNatN->events = 0;
            fdsN->autoremove = 1;
            obj->poll.autoremovesPend++;
            r = 0;
            break;
        }
    }
    return r;
}

//remove inmediatly (unsafe inside poll-events)
/*int Player_pollRemove_(STPlayer* obj, const ENPlayerPollFdType type, const void* objPtr, const int fd){
    int r = -1;
    //search
    int i; for(i = 0; i < obj->poll.fdsUse; i++){
        STPlayerPollFd* fdsN = &obj->poll.fds[i];
        struct pollfd* fdsNatN = &obj->poll.fdsNat[i];
        if(fdsN->type == type && fdsN->obj == objPtr && fdsNatN->fd == fd){
            //remove
            obj->poll.fdsUse--;
            for(;i < obj->poll.fdsUse; i++){
                obj->poll.fds[i] = obj->poll.fds[i + 1];
                obj->poll.fdsNat[i] = obj->poll.fdsNat[i + 1];
            }
            r = 0;
            break;
        }
    }
    return r;
}*/

//fbs

int Player_fbAdd(STPlayer* obj, const char* device, const int animSecsWaits){
    int r = -1;
    //resize arr
    while(obj->fbs.arrUse >= obj->fbs.arrSz){
        STFramebuff** arrN = NULL;
        //
        arrN = malloc(sizeof(STFramebuff*) * (obj->fbs.arrSz + 1));
        if(arrN == NULL){
            if(arrN != NULL) free(arrN); arrN = NULL;
            break;
        } else {
            obj->fbs.arrSz++;
            if(obj->fbs.arr != NULL){
                if(obj->fbs.arrUse > 0){
                    memcpy(arrN, obj->fbs.arr, sizeof(STFramebuff*) * obj->fbs.arrUse);
                }
                free(obj->fbs.arr);
            }
            obj->fbs.arr = arrN;
        }
    }
    //add record
    if((obj->fbs.arrUse + 1) <= obj->fbs.arrSz){
        STFramebuff* fbN = malloc(sizeof(STFramebuff));
        Framebuff_init(fbN);
        r = 0;
        if(0 != Framebuff_open(fbN, device, animSecsWaits)){
            printf("ERROR, Player, fbAdd failed: '%s'.\n", device);
            r = -1;
        } else {
            printf("Player_fbAdd device added to poll: '%s'.\n", device);
            //consume
            obj->fbs.arr[obj->fbs.arrUse] = fbN; fbN = NULL;
            obj->fbs.arrUse++;
            //reorganize
            if(0 != Player_organize(obj)){
                printf("ERROR, Player_organize failed after fb creation.\n");
            }
        }
        //release (if not consumed)
        if(fbN != NULL){
            Framebuff_release(fbN);
            free(fbN);
            fbN = NULL;
        }
    }
    //
    return r;
}

int Player_fbRemove(STPlayer* obj, STFramebuff* fb){
    int r = -1;
    //search
    int i; for(i = 0; i < obj->fbs.arrUse; i++){
        if(fb == obj->fbs.arr[i]){
            //remove
            obj->fbs.arrUse--;
            for(;i < obj->fbs.arrUse; i++){
                obj->fbs.arr[i] = obj->fbs.arr[i + 1];
            }
            Framebuff_release(fb);
            free(fb);
            r = 0;
            //reorganize
            if(0 != Player_organize(obj)){
                printf("ERROR, Player_organize failed after fb removal.\n");
            }
            break;
        }
    }
    return r;
}


//streams

int Player_streamAdd(STPlayer* obj, const char* device, const char* server, const unsigned int port, const char* resPath, const int connTimeoutSecs, const int decoderTimeoutSecs){
    int r = -1;
    //resize arr
    while(obj->streams.arrUse >= obj->streams.arrSz){
        STStreamContext** arrN = NULL;
        //
        arrN = malloc(sizeof(STStreamContext*) * (obj->streams.arrSz + 1));
        if(arrN == NULL){
            if(arrN != NULL) free(arrN); arrN = NULL;
            break;
        } else {
            obj->streams.arrSz++;
            if(obj->streams.arr != NULL){
                if(obj->streams.arrUse > 0){
                    memcpy(arrN, obj->streams.arr, sizeof(STStreamContext*) * obj->streams.arrUse);
                }
                free(obj->streams.arr);
            }
            obj->streams.arr = arrN;
        }
    }
    //add record
    if((obj->streams.arrUse + 1) <= obj->streams.arrSz){
        //try every framebuffer format
        int lastPixFmt = 0, streamAdded = 0;
        int i; for(i = 0; i < obj->fbs.arrUse && !streamAdded; i++){
            STFramebuff* fb = obj->fbs.arr[i];
            if(lastPixFmt != fb->pixFmt){
                STStreamContext* streamN = malloc(sizeof(STStreamContext));
                StreamContext_init(streamN);
                r = 0;
                if(0 != StreamContext_open(streamN, obj, device, server, port, resPath, V4L2_PIX_FMT_H264, 1, 1, (1024 * 1024 * 2), fb->pixFmt, connTimeoutSecs, decoderTimeoutSecs)){
                    printf("ERROR, Player, streamAdd failed: '%s' @ '%s'.\n", resPath, device);
                    r = -1;
                } else {
                    printf("Player_streamAdd device added to poll: '%s'.\n", resPath);
                    //consume
                    streamN->streamId = ++obj->streamIdNext;
                    obj->streams.arr[obj->streams.arrUse] = streamN; streamN = NULL;
                    obj->streams.arrUse++;
                    streamAdded = 1;
                    //reorganize
                    if(0 != Player_organize(obj)){
                        printf("ERROR, Player_organize failed after stream creation.\n");
                    }
                }
                //release (if not consumed)
                if(streamN != NULL){
                    StreamContext_release(streamN);
                    free(streamN);
                    streamN = NULL;
                }
                //
                lastPixFmt = fb->pixFmt;
            }
        }
    }
    //
    return r;
}

int Player_streamRemove(STPlayer* obj, STStreamContext* stream){
    int r = -1;
    //search
    int i; for(i = 0; i < obj->streams.arrUse; i++){
        if(stream == obj->streams.arr[i]){
            //remove
            obj->streams.arrUse--;
            for(;i < obj->streams.arrUse; i++){
                obj->streams.arr[i] = obj->streams.arr[i + 1];
            }
            StreamContext_release(stream);
            free(stream);
            r = 0;
            //reorganize
            if(0 != Player_organize(obj)){
                printf("ERROR, Player_organize failed after stream removal.\n");
            }
            break;
        }
    }
    return r;
}

//organize
//Assigns fb and screen-area to each stream. Should be called on each stream resize event.
int Player_organize(STPlayer* obj){
    int r = 0;
    int i; for(i = 0 ; i < obj->fbs.arrUse; i++){
        STFramebuff* fb = obj->fbs.arr[i];
        if(0 != Framebuff_layoutStart(fb)){
            printf("ERROR, Framebuff_layoutStart failed.\n");
        } else {
            int j; for(j = 0; j < obj->streams.arrUse; j++){
                STStreamContext* s = obj->streams.arr[j];
                //ToDo: validate: s->dec.dst.pixelformat == fb->pixFmt.
                /*if(s->dec.dst.pixelformat == fb->pixFmt && s->dec.dst.composition.width > 0 && s->dec.dst.composition.height > 0)*/
                {
                    STFbSize sz;
                    memset(&sz, 0, sizeof(sz));
                    sz.width = s->dec.dst.composition.width;
                    sz.height = s->dec.dst.composition.height;
                    if(0 != Framebuff_layoutAdd(fb, s->streamId, sz)){
                        printf("ERROR, Framebuff_layoutAdd failed.\n");
                    }
                }
            }
            //
            if(0 != Framebuff_layoutEnd(fb)){
                printf("ERROR, Framebuff_layoutEnd failed.\n");
            }
        }
    }
    return r;
}

int Player_tick(STPlayer* obj, int ms){
    int r = 0;
    //fbs
    {
        int i; for(i = 0 ; i < obj->fbs.arrUse; i++){
            STFramebuff* fb = obj->fbs.arr[i];
            if(0 != Framebuff_animTick(fb, ms)){
                printf("ERROR, Framebuff_animTick failed.\n");
            }
            //draw
            {
                int drawn = 0;
                struct timeval start;
                gettimeofday(&start, NULL);
                if(1){
                    //--
                    //direct to screen
                    //--
                    //sync offscreen
                    if(!fb->offscreen.isSynced){
                        if(0 != Framebuff_drawToPtr(fb, obj, &fb->screen)){
                            printf("ERROR, Framebuff_drawToPtr failed.\n");
                        } else {
                            drawn = 1;
                        }
                        fb->offscreen.isSynced = 1;
                        fb->screen.isSynced = 1;
                    }
                } else {
                    //--
                    //use offscreen
                    //--
                    //sync offscreen
                    if(!fb->offscreen.isSynced){
                        if(0 != Framebuff_drawToPtr(fb, obj, &fb->offscreen)){
                            printf("ERROR, Framebuff_drawToPtr failed.\n");
                        } else {
                            drawn = 1;
                        }
                        fb->offscreen.isSynced = 1;
                    }
                    //sync screen
                    if(!fb->screen.isSynced){
                        if(fb->screen.ptr != NULL && fb->offscreen.ptr && fb->screen.ptrSz == fb->offscreen.ptrSz){
                            memcpy(fb->screen.ptr, fb->offscreen.ptr, fb->screen.ptrSz);
                        }
                        fb->screen.isSynced = 1;
                        drawn = 1;
                    }
                }
                //add stats
                if(drawn){
                    struct timeval end; long ms;
                    gettimeofday(&end, NULL);
                    ms = msBetweenTimevals(&start, &end);
                    if(ms >= 0){
                        pthread_mutex_lock(&obj->stats.mutex);
                        {
                            if(obj->stats.curSec.drawCount == 0){
                                obj->stats.curSec.drawMsMin = ms;
                                obj->stats.curSec.drawMsMax = ms;
                            } else {
                                if(obj->stats.curSec.drawMsMin > ms) obj->stats.curSec.drawMsMin = ms;
                                if(obj->stats.curSec.drawMsMax < ms) obj->stats.curSec.drawMsMax = ms;
                            }
                            obj->stats.curSec.drawMsSum += ms;
                            obj->stats.curSec.drawCount++;
                        }
                        pthread_mutex_unlock(&obj->stats.mutex);
                    }
                }
            }
        }
    }
    //streams
    {
        int i; for(i = (int)obj->streams.arrUse - 1; i >= 0; i--){
            StreamContext_tick(obj->streams.arr[i], obj, ms);
        }
    }
    //
    {
        obj->msRunning += ms;
    }
    return r;
}

//-------------------
//-- StreamContext --
//-------------------

void StreamContext_init(STStreamContext* ctx){
    memset(ctx, 0, sizeof(STStreamContext));
    //net
    {
        //buff
        {
            ctx->net.buff.buffUse   = 0;
            ctx->net.buff.buffSz    = (1024 * 64);
            ctx->net.buff.buff      = (unsigned char*)malloc(ctx->net.buff.buffSz);
        }
    }
    //dec (decoder)
    {
        ctx->dec.fd = -1;
        //src
        {
            Buffers_init(&ctx->dec.src);
        }
        //dst
        {
            Buffers_init(&ctx->dec.dst);
        }
        //frames
        {
            //
        }
    }
    //frames
    {
        VideoFrames_init(&ctx->frames.filled);      //filled with payload
        VideoFrames_init(&ctx->frames.reusable);    //for reutilization
    }
}

void StreamContext_release(STStreamContext* ctx){
    //net
    {
        //buff
        {
            if(ctx->net.buff.buff != NULL){
                free(ctx->net.buff.buff);
                ctx->net.buff.buff = NULL;
            }
            ctx->net.buff.buffSz = 0;
        }
        //req
        {
            if(ctx->net.req.pay != NULL){
                free(ctx->net.req.pay);
                ctx->net.req.pay = NULL;
            }
            ctx->net.req.payUse = 0;
            ctx->net.req.paySz = 0;
        }
    }
    //frames
    {
        //currently filling
        if(ctx->frames.filling != NULL){
            VideoFrame_release(ctx->frames.filling);
            free(ctx->frames.filling);
            ctx->frames.filling = NULL;
        }
        VideoFrames_release(&ctx->frames.filled);      //filled with payload
        VideoFrames_release(&ctx->frames.reusable);    //for reutilization
    }
    //dec (decoder)
    {
        if(0 != StreamContext_stopAndUnmapBuffs(ctx, &ctx->dec.dst)){
            printf("WARNING, StreamContext_stopAndUnmapBuffs(dst) failed.\n");
        }
        if(0 != StreamContext_stopAndUnmapBuffs(ctx, &ctx->dec.src)){
            printf("WARNING, StreamContext_stopAndUnmapBuffs(src) failed.\n");
        }
        if(0 != StreamContext_eventsUnsubscribe(ctx, ctx->dec.fd)){
            printf("ERROR, StreamContext, unsubscribe failed.\n");
        }
        if(ctx->dec.fd >= 0){
            v4l2_close(ctx->dec.fd);
            ctx->dec.fd = -1;
        }
        Buffers_release(&ctx->dec.src);
        Buffers_release(&ctx->dec.dst);
        //frames
        {
            VideoFrameStates_release(&ctx->dec.frames.fed);
        }
    }
    //cfg
    {
        if(ctx->cfg.device != NULL){ free(ctx->cfg.device); ctx->cfg.device = NULL; }
        if(ctx->cfg.server != NULL){ free(ctx->cfg.server); ctx->cfg.server = NULL; }
        if(ctx->cfg.path != NULL){ free(ctx->cfg.path); ctx->cfg.path = NULL; }
    }
}

int StreamContext_concatHttpRequest(STStreamContext* ctx, char* dst, const int dstSz){
    int r = 0;
    //line-0
    {
        const char* str = "GET ";
        const int len = strlen(str);
        if(dst != NULL && (r + len) <= dstSz){
            memcpy(&dst[r], str, len);
        }
        r += len;
    }
    {
        const char* str = ctx->cfg.path;
        const int len = strlen(str);
        if(dst != NULL && (r + len) <= dstSz){
            memcpy(&dst[r], str, len);
        }
        r += len;
    }
    {
        const char* str = " HTTP/1.1\r\n";
        const int len = strlen(str);
        if(dst != NULL && (r + len) <= dstSz){
            memcpy(&dst[r], str, len);
        }
        r += len;
    }
    //line-1
    {
        const char* str = "Host: ";
        const int len = strlen(str);
        if(dst != NULL && (r + len) <= dstSz){
            memcpy(&dst[r], str, len);
        }
        r += len;
    }
    {
        const char* str = ctx->cfg.server;
        const int len = strlen(str);
        if(dst != NULL && (r + len) <= dstSz){
            memcpy(&dst[r], str, len);
        }
        r += len;
    }
    {
        const char* str = "\r\n";
        const int len = strlen(str);
        if(dst != NULL && (r + len) <= dstSz){
            memcpy(&dst[r], str, len);
        }
        r += len;
    }
    //line-2
    {
        const char* str = "\r\n";
        const int len = strlen(str);
        if(dst != NULL && (r + len) <= dstSz){
            memcpy(&dst[r], str, len);
        }
        r += len;
    }
    return r;
}

int StreamContext_getMinBuffersForDst(STStreamContext* ctx, int* dstValue){
    struct v4l2_control ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE;
    const int rr2 = v4l2_ioctl(ctx->dec.fd, VIDIOC_G_CTRL, &ctrl);
    if(rr2 == 0 && dstValue != NULL){
        *dstValue = ctrl.value;
    }
    return rr2;
}

int StreamContext_initAndPrepareSrc(STStreamContext* ctx, int fd, const int buffersAmmount, const int print){
    int r = -1;
    //should be stopped
    if(0 != Buffers_allocBuffs(&ctx->dec.src, fd, buffersAmmount, print)){
        printf("ERROR, StreamContext, Buffers_allocBuffs(%d) failed.\n", buffersAmmount);
    } else if(ctx->dec.src.use <= 0){
        printf("ERROR, StreamContext, Buffers_allocBuffs(%d) created zero buffs.\n", buffersAmmount);
    //} else if(0 != Buffers_export(&ctx->dec.src, fd)){
    //    printf("ERROR, StreamContext, Buffers_export(%d) failed.\n", ctx->dec.src.use);
    } else if(0 != Buffers_mmap(&ctx->dec.src, fd)){
        printf("ERROR, StreamContext, Buffers_mmap(%d) failed.\n", ctx->dec.src.use);
    } else {
        if(ctx->dec.src.use == buffersAmmount){
            printf("StreamContext, inited device with %d buffers.\n", buffersAmmount);
        } else {
            printf("StreamContext, inited device with %d of %d buffers.\n", ctx->dec.src.use, buffersAmmount);
        }
        ctx->dec.isWaitingForIDRFrame = 1;
        r = 0;
    }
    return r;
}

int StreamContext_initAndStartDst(STStreamContext* ctx, struct STPlayer_* plyr){
    int r = -1;
    STPrintedInfo* printDstFmt = Player_getPrintIfNotRecent(plyr, ctx->cfg.device, ctx->cfg.srcPixFmt, ctx->cfg.dstPixFmt, K_DEF_REPRINTS_HIDE_SECS);
    if(printDstFmt != NULL){ PrintedInfo_touch(printDstFmt); }
    ctx->dec.dst.enqueuedRequiredMin = 0;
    if(0 != Buffers_setFmt(&ctx->dec.dst, ctx->dec.fd, ctx->cfg.dstPixFmt, 1, 0, (printDstFmt != NULL ? 1 : 0))){
        printf("ERROR, Buffers_setFmt(dst) failed: '%s'.\n", ctx->cfg.device);
    } else if(0 != StreamContext_getMinBuffersForDst(ctx, &ctx->dec.dst.enqueuedRequiredMin)){
        printf("ERROR, StreamContext_getMinBuffersForDst(dst) failed: '%s'.\n", ctx->cfg.device);
    } else if(ctx->dec.dst.enqueuedRequiredMin <= 0){
        printf("ERROR, StreamContext_getMinBuffersForDst(dst) returned(%d): '%s'.\n", ctx->dec.dst.enqueuedRequiredMin, ctx->cfg.device);
    } else if(0 != Buffers_allocBuffs(&ctx->dec.dst, ctx->dec.fd, ctx->dec.dst.enqueuedRequiredMin + 1, (printDstFmt != NULL ? 1 : 0))){ //+1 to keep a copy for rendering
        printf("ERROR, Buffers_allocBuffs(%d, dst) failed: '%s'.\n", (ctx->dec.dst.enqueuedRequiredMin + 1), ctx->cfg.device);
    } else if(ctx->dec.dst.use <= 0){
        printf("ERROR, Buffers_allocBuffs(%d, dst) created zero buffers: '%s'.\n", ctx->dec.dst.use, ctx->cfg.device);
    } else if(ctx->dec.dst.use < ctx->dec.dst.enqueuedRequiredMin){
        printf("ERROR, Buffers_allocBuffs(%d, dst) created below minimun(%d) buffers: '%s'.\n", ctx->dec.dst.use, ctx->dec.dst.enqueuedRequiredMin, ctx->cfg.device);
    //} else if(0 != Buffers_export(&ctx->dec.dst, ctx->dec.fd)){
    //    printf("ERROR, Buffers_export(%d, dst) failed: '%s'.\n", ctx->dec.dst.use, ctx->cfg.device);
    } else if(0 != Buffers_mmap(&ctx->dec.dst, ctx->dec.fd)){
        printf("ERROR, Buffers_mmap(%d, dst) failed: '%s'.\n", ctx->dec.dst.use, ctx->cfg.device);
    } else if(0 != Buffers_enqueueMinimun(&ctx->dec.dst, ctx->dec.fd, ctx->dec.dst.enqueuedRequiredMin)){
        printf("ERROR, Buffers_enqueueMinimun(%d / %d, dst) failed: '%s'.\n", ctx->dec.dst.enqueuedRequiredMin, ctx->dec.dst.use, ctx->cfg.device);
    } else if(0 != Buffers_start(&ctx->dec.dst, ctx->dec.fd)){
        printf("ERROR, Buffers_start(%d, dst) failed: '%s'.\n", ctx->dec.dst.use, ctx->cfg.device);
    } else {
        if(ctx->dec.dst.use == (ctx->dec.dst.enqueuedRequiredMin + 1)){
            printf("StreamContext, dst-started (%d buffers): '%s'.\n", (ctx->dec.dst.enqueuedRequiredMin + 1), ctx->cfg.device);
        } else {
            printf("StreamContext, dst-started (%d/%d buffers): '%s'.\n", ctx->dec.dst.use, (ctx->dec.dst.enqueuedRequiredMin + 1), ctx->cfg.device);
        }
        if(ctx->dec.dst.use == ctx->dec.dst.enqueuedRequiredMin){
            printf("WARNING, attempt to allocate one extra buffer failed, this implies an extra memcopy() per decoded-frame: '%s'.\n", ctx->cfg.device);
        }
        r = 0;
        //update poll (dst started)
        StreamContext_updatePollMask_(ctx, plyr);
        //reorganize
        if(0 != Player_organize(plyr)){
            printf("ERROR, Player_organize failed after dst-resized.\n");
        }
    }
    return r;
}

int StreamContext_stopAndUnmapBuffs(STStreamContext* ctx, STBuffers* buffs){
    int r = -1;
    //IMPORTANT NOTE: if device exposed 'V4L2_BUF_CAP_SUPPORTS_ORPHANED_BUFS' capability
    //, the buffer is not released untill is unmaped and closed.
    //
    if(buffs->isExplicitON){
        if(0 != Buffers_stop(buffs, ctx->dec.fd)){
            printf("ERROR, Buffers_stop(dst) failed: '%s'.\n", ctx->cfg.device);
        }
    }
    //
    if(0 != Buffers_unmap(buffs, ctx->dec.fd)){
        printf("ERROR, Buffers_unmap(dst) failed: '%s'.\n", ctx->cfg.device);
    }
    //
    if(0 != Buffers_allocBuffs(buffs, ctx->dec.fd, 0, 0)){
        printf("ERROR, Buffers_allocBuffs(dst, 0) failed: '%s'.\n", ctx->cfg.device);
    } else {
        r = 0;
    }
    return r;
}

//

int StreamContext_eventsSubscribe(STStreamContext* ctx, int fd){
    //V4L2_EVENT_RESOLUTION_CHANGE == V4L2_EVENT_SOURCE_CHANGE
    //V4L2_EVENT_SOURCE_CHANGE (event with changes set to V4L2_EVENT_SRC_CH_RESOLUTION)
    {
        struct v4l2_event_subscription sub;
        memset(&sub, 0, sizeof(sub));
        //
        sub.type = V4L2_EVENT_SOURCE_CHANGE;
        //
        int rr; CALL_IOCTL(rr, v4l2_ioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &sub));
        if(rr != 0){
            printf("ERROR, StreamContext, Subscription to event V4L2_EVENT_SOURCE_CHANGE returned(%d).\n", rr);
            return -1;
        } else {
            printf("StreamContext, Subscription to event V4L2_EVENT_SOURCE_CHANGE success.\n");
        }
    }
    //V4L2_EVENT_EOS
    {
        struct v4l2_event_subscription sub;
        memset(&sub, 0, sizeof(sub));
        //
        sub.type = V4L2_EVENT_EOS;
        //
        int rr; CALL_IOCTL(rr, v4l2_ioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &sub));
        if(rr != 0){
            printf("ERROR, StreamContext, Subscription to event V4L2_EVENT_EOS returned(%d).\n", rr);
            return -1;
        } else {
            printf("StreamContext, Subscription to event V4L2_EVENT_EOS success.\n");
        }
    }
    return 0;
}

int StreamContext_eventsUnsubscribe(STStreamContext* ctx, int fd){
    struct v4l2_event_subscription sub;
    memset(&sub, 0, sizeof(sub));
    //
    sub.type = V4L2_EVENT_ALL;
    //
    int rr; CALL_IOCTL(rr, v4l2_ioctl(fd, VIDIOC_UNSUBSCRIBE_EVENT, &sub));
    if(rr != 0){
        printf("ERROR, StreamContext, Unscubscribe failed.\n");
        return -1;
    } else {
        printf("StreamContext, Unscubscribe success.\n");
    }
    return 0;
}


void StreamContext_tick(STStreamContext* ctx, struct STPlayer_* plyr, unsigned int ms){
    //decoder
    if(ctx->dec.fd < 0){
        if(ctx->dec.msToReopen <= ms){
            ctx->dec.msToReopen = 0;
        } else {
            ctx->dec.msToReopen -= ms;
            if((ctx->dec.msToReopen / 1000) != ((ctx->dec.msToReopen + ms) / 1000)){
                printf("StreamContext_tick, waiting %d secs to reopen attempt: %s.\n", (ctx->dec.msToReopen / 1000), ctx->cfg.path);
            }
        }
        //reopen
        if(ctx->dec.msToReopen == 0){
            //ToDo: execute this action once no src-buffer is queued or timeedout src-bufer flush.
            if(0 != StreamContext_open(ctx, plyr, ctx->cfg.device, ctx->cfg.server, ctx->cfg.port, ctx->cfg.path, ctx->cfg.srcPixFmt, ctx->cfg.buffersAmmount, ctx->cfg.planesPerBuffer, ctx->cfg.sizePerPlane, ctx->cfg.dstPixFmt, ctx->cfg.connTimeoutSecs, ctx->cfg.decoderTimeoutSecs)){
                printf("ERROR, Player, streamAdd failed: '%s' @ '%s'.\n", ctx->cfg.path, ctx->cfg.device);
            } else {
                printf("StreamContext_tick device reopened and added to poll: '%s'.\n", ctx->cfg.path);
            }
            ctx->dec.msToReopen = (plyr->cfg.decoderWaitRecopenSecs <= 0 ? 1 : plyr->cfg.decoderWaitRecopenSecs) * 1000;
        }
    }
    //net
    if(ctx->net.socket > 0){
        int simConnTimeout = 0;
        //
        if(plyr->cfg.dbg.simNetworkTimeout > 0){
            if((rand() % plyr->cfg.dbg.simNetworkTimeout) == 0){
                printf("WARNING, StreamContext_tick, forcing/simulating a NETWORK timeout (1 / %d prob.): '%s'.\n", plyr->cfg.dbg.simNetworkTimeout, ctx->cfg.path);
                simConnTimeout = 1;
            }
        }
        //connecting or connected
        ctx->net.msWithoutSend += ms;
        ctx->net.msWithoutRecv += ms;
        //
        if(simConnTimeout || (ctx->cfg.connTimeoutSecs > 0 && ctx->net.msWithoutSend > (ctx->cfg.connTimeoutSecs * 1000) && ctx->net.msWithoutRecv > (ctx->cfg.connTimeoutSecs * 1000))){
            if(simConnTimeout){
                printf("ERROR, net, simulated-connection-timeout('%s:%d') after %ds not writting and %ds not reading: '%s'.\n", ctx->cfg.server, ctx->cfg.port, (ctx->net.msWithoutSend / 1000), (ctx->net.msWithoutRecv / 1000), ctx->cfg.path);
            } else if(ctx->net.msWithoutSend == ctx->net.msWithoutRecv){
                //the lastest inactive trigger was both (connection attemp)
                printf("ERROR, net, connection-timeout('%s:%d') after %ds: '%s'.\n", ctx->cfg.server, ctx->cfg.port, (ctx->net.msWithoutSend / 1000), ctx->cfg.path);
            } else if(ctx->net.msWithoutSend < ctx->net.msWithoutRecv){
                //the lastest inactive trigger was writting
                printf("ERROR, net, connection-timeout('%s:%d') after %ds not writting: '%s'.\n", ctx->cfg.server, ctx->cfg.port, (ctx->net.msWithoutSend / 1000), ctx->cfg.path);
            } else {
                //the lastest inactive trigger was reading
                printf("ERROR, net, connection-timeout('%s:%d') after %ds not reading: '%s'.\n", ctx->cfg.server, ctx->cfg.port, (ctx->net.msWithoutRecv / 1000), ctx->cfg.path);
            }
            if(ctx->net.socket){
                Player_pollAutoRemove(plyr, ENPlayerPollFdType_SrcSocket, ctx, ctx->net.socket);
                close(ctx->net.socket);
                ctx->net.socket = 0;
            }
        }
    } else {
        //waiting
        if(ctx->net.msToReconnect <= ms){
            ctx->net.msToReconnect = 0;
        } else {
            ctx->net.msToReconnect -= ms;
            if((ctx->net.msToReconnect / 1000) != ((ctx->net.msToReconnect + ms) / 1000)){
                printf("StreamContext_tick, waiting %d secs to reconnect attempt: '%s'.\n", (ctx->net.msToReconnect / 1000), ctx->cfg.path);
            }
        }
        //reconnect
        if(ctx->net.msToReconnect == 0){
            ctx->net.msToReconnect = (plyr->cfg.connWaitReconnSecs > 0 ? plyr->cfg.connWaitReconnSecs : 1) * 1000;
            //
            struct in_addr hostAddr; int hostAddrFnd = 0;
            memset(&hostAddr, 0, sizeof(struct in_addr));
            //Get adddress (global lock; only one parallel call to 'gethostbyname()' is allowed)
            {
                struct addrinfo hints, * result;
                memset(&hints, 0, sizeof(struct addrinfo));
                hints.ai_family     = PF_UNSPEC;
                hints.ai_socktype   = SOCK_STREAM;
                hints.ai_flags      |= AI_CANONNAME;
                if (0 == getaddrinfo(ctx->cfg.server, NULL, &hints, &result)){
                    struct addrinfo* res = result;
                    while (res){
                        if (res->ai_family == AF_INET) {
                            hostAddr = ((struct sockaddr_in*)res->ai_addr)->sin_addr;
                            hostAddrFnd = 1;
                            break;
                            //} else if (res->ai_family == AF_INET6) {
                            //ToDo: enable 'sockaddr_in6'
                            //ptr = &((struct sockaddr_in6*)res->ai_addr)->sin6_addr;
                            //hostAddrFnd = 1;
                            //break;
                        }
                        //next
                        res = res->ai_next;
                    }
                    freeaddrinfo(result);
                }
                /*
                 //The functions gethostbyname() and gethostbyaddr()
                 //may return pointers to static data,
                 //which may be overwritten by later calls.
                 //Copying the struct hostent does not suffice,
                 //since it contains pointers; a deep copy is required.
                 struct hostent* host = gethostbyname(server);
                 if(host && host->h_addr_list != NULL && host->h_addr_list[0] != NULL) {
                 hostAddr = *((struct in_addr*)host->h_addr_list[0]);
                 hostAddrFnd = 1;
                 }
                 */
            }
            //Connect
            {
                //Connect
                if(!hostAddrFnd){
                    printf("ERROR, net, host-addr-not-found('%s'): '%s'.\n", ctx->cfg.server, ctx->cfg.path);
                } else {
                    struct sockaddr remoteAddr;
                    memset(&remoteAddr, 0, sizeof(struct sockaddr));
                    {
                        struct sockaddr_in* addr4 = (struct sockaddr_in*)&remoteAddr;
                        addr4->sin_family    = AF_INET;
                        addr4->sin_port      = (u_short)htons((u_short)ctx->cfg.port);
                        addr4->sin_addr      = hostAddr;
                    }
                    //Create hnd (if necesary)
                    SOCKET sckt = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                    if(!sckt || sckt == INVALID_SOCKET){
                        printf("ERROR, net, socket creation failed: '%s'.\n", ctx->cfg.path);
                        ctx->net.socket = 0;
                    }
                    //config
#                   ifdef SO_NOSIGPIPE
                    if(sckt && sckt != INVALID_SOCKET){
                        int v = 1; //(noSIGPIPE ? 1 : 0);
                        if(setsockopt(sckt, SOL_SOCKET, SO_NOSIGPIPE, (const char*)&v, sizeof(v)) < 0){
                            printf("ERROR, net, socket SO_NOSIGPIPE option failed: '%s'.\n", ctx->cfg.path);
                            close(sckt);
                            sckt = 0;
                        }
                    }
#                   endif
                    //non-block
                    if(sckt && sckt != INVALID_SOCKET){
                        int flags, nonBlocking = 1;
                        if ((flags = fcntl(sckt, F_GETFL, 0)) == -1){
                            printf("ERROR, net, F_GETFL failed: '%s'.\n", ctx->cfg.path);
                            close(sckt);
                            sckt = 0;
                        } else {
#                           ifdef O_NONBLOCK
                            if(nonBlocking) flags |= O_NONBLOCK;
                            else flags &= ~O_NONBLOCK;
#                           endif
#                           ifdef O_NDELAY
                            if(nonBlocking) flags |= O_NDELAY;
                            else flags &= ~O_NDELAY;
#                           endif
#                           ifdef FNDELAY
                            if(nonBlocking) flags |= FNDELAY;
                            else flags &= ~FNDELAY;
#                           endif
                            if(fcntl(sckt, F_SETFL, flags) == -1){
                                printf("ERROR, net, F_SETFL O_NONBLOCK option failed: '%s'.\n", ctx->cfg.path);
                                close(sckt);
                                sckt = 0;
                            }
                        }
                    }
                    //connect
                    if(sckt && sckt != INVALID_SOCKET){
                        //Connect (unlocked)
                        {
                            const int nret = connect(sckt, (struct sockaddr*)&remoteAddr, sizeof(remoteAddr));
                            if (nret != 0){
                                //EINPROGRESS: first call; EALREADY: subsequent calls
                                if(!(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS)){
                                    printf("ERROR, connect-start-failed to '%s:%d': '%s'.\n", ctx->cfg.server, ctx->cfg.port, ctx->cfg.path);
                                    close(sckt);
                                    sckt = 0;
                                } else {
                                    printf("Net, connect-started to '%s:%d': '%s'.\n", ctx->cfg.server, ctx->cfg.port, ctx->cfg.path);
                                }
                            }
                        }
                        //add to pollster
                        if(0 != Player_pollAdd(plyr, ENPlayerPollFdType_SrcSocket, StreamContext_pollCallback, ctx, sckt, POLLOUT)){ //write
                            printf("ERROR, poll-add-failed to '%s:%d': '%s'.\n", ctx->cfg.server, ctx->cfg.port, ctx->cfg.path);
                            close(sckt);
                            sckt = 0;
                        } else {
                            printf("StreamContext, socket added to poll: '%s:%d': '%s'.\n", ctx->cfg.server, ctx->cfg.port, ctx->cfg.path);
                            ctx->net.socket = sckt;
                            ctx->net.msWithoutSend = 0;
                            ctx->net.msWithoutRecv = 0;
                        }
                    }
                    //build request
                    {
                        const int reqSz = StreamContext_concatHttpRequest(ctx, ctx->net.req.pay, ctx->net.req.paySz);
                        if((reqSz + 1) <= ctx->net.req.paySz){ //+1 for '\0' for printing
                            ctx->net.req.pay[reqSz] = '\0';
                            ctx->net.req.payUse     = reqSz;
                            ctx->net.req.payCsmd    = 0;
                            printf("Net http-req built (reused %d/%d buffer): '%s'.\n", ctx->net.req.payUse, ctx->net.req.paySz, ctx->cfg.path);
                        } else {
                            if(ctx->net.req.pay != NULL){
                                free(ctx->net.req.pay);
                                ctx->net.req.pay = NULL;
                            }
                            ctx->net.req.paySz      = reqSz + 1; //+1 for '\0' for printing
                            ctx->net.req.pay        = (char*)malloc(ctx->net.req.paySz);
                            ctx->net.req.payUse     = StreamContext_concatHttpRequest(ctx, ctx->net.req.pay, ctx->net.req.paySz);
                            ctx->net.req.payCsmd    = 0;
                            ctx->net.req.pay[ctx->net.req.payUse] = '\0';
                            printf("Net http-req built (new %d/%d buffer): '%s'.\n", ctx->net.req.payUse, ctx->net.req.paySz, ctx->cfg.path);
                        }
                    }
                    //reset vars
                    {
                        ctx->net.resp.headerEndSeq = 0;
                        ctx->net.resp.headerSz = 0;
                        ctx->net.resp.headerEnded = 0;
                        //
                        ctx->net.buff.buffUse = 0;
                        ctx->net.buff.buffCsmd = 0;
                        //
                        ctx->net.resp.nal.zeroesSeqAccum    = 0;   //reading posible headers '0x00 0x00 0x00 0x01' (start of a NAL)
                        //ctx->net.resp.nal.startsCount = 0;    //total NALs found
                        //
                        ctx->frames.fillingNalSz = 0;
                        if(ctx->frames.filling != NULL){
                            //add for future pull (reuse)
                            if(0 != VideoFrames_pushFrameOwning(&ctx->frames.reusable, ctx->frames.filling)){
                                printf("StreamContext_tick, VideoFrames_pushFrameOwning failed: '%s'.\n", ctx->cfg.path);
                                VideoFrame_release(ctx->frames.filling);
                                free(ctx->frames.filling);
                            }
                            ctx->frames.filling = NULL;
                        }
                    }
                }
            }
        }
    }
    //buffs
    if(ctx->dec.fd >= 0){
        int simDecoderTimeout = 0;
        if(plyr->cfg.dbg.simDecoderTimeout > 0){
            if((rand() % plyr->cfg.dbg.simDecoderTimeout) == 0){
                printf("WARNING, StreamContext_tick, forcing/simulating a DECODER timeout (1 / %d prob.): '%s'.\n", plyr->cfg.dbg.simDecoderTimeout, ctx->cfg.path);
                simDecoderTimeout = 1;
            }
        }
        //
        ctx->dec.src.msWithoutEnqueuing += ms;
        ctx->dec.src.msWithoutDequeuing += ms;
        //
        ctx->dec.dst.msWithoutEnqueuing += ms;
        ctx->dec.dst.msWithoutDequeuing += ms;
        //
        //Detect decoder timeout
        if(plyr->cfg.decoderTimeoutSecs > 0 || simDecoderTimeout){
            if(ctx->dec.src.isExplicitON && ctx->dec.dst.isExplicitON && (simDecoderTimeout || (ctx->dec.dst.msWithoutDequeuing > ctx->dec.msWithoutFeedFrame && (ctx->dec.dst.msWithoutDequeuing - ctx->dec.msWithoutFeedFrame) >= (plyr->cfg.decoderTimeoutSecs * 1000)))){
                unsigned long msDecoderInative = (ctx->dec.dst.msWithoutDequeuing - ctx->dec.msWithoutFeedFrame);
                if(simDecoderTimeout){
                    printf("ERROR, StreamContext_tick, simulated-decoder timeout: %u ms inactive while ON and frames arriving: '%s'.\n", msDecoderInative, ctx->cfg.path);
                } else {
                    printf("ERROR, StreamContext_tick, decoder timeout: %u ms inactive while ON and frames arriving: '%s'.\n", msDecoderInative, ctx->cfg.path);
                }
                {
                    //IMPORTANT: in raspberry pi-4 'seek' is aparently unsupported,
                    //           leaking buffers and producing messages
                    //           "bcm2835_codec_flush_buffers: Timeout waiting for buffers to be returned".
                    //           Instead of stopping and resuming the src-buffers, is safer to reopen the device file.
                    //
                    //close
                    if(0 != StreamContext_close(ctx, plyr)){
                        printf("WARNING, StreamContext_close failed: '%s'.\n", ctx->cfg.path);
                    }
                    ctx->dec.isWaitingForIDRFrame = 1;
                    ctx->dec.msToReopen = (plyr->cfg.decoderWaitRecopenSecs <= 0 ? 1 : plyr->cfg.decoderWaitRecopenSecs) * 1000;
                }
                //reset (just in case)
                {
                    ctx->dec.src.msWithoutEnqueuing = 0;
                    ctx->dec.src.msWithoutDequeuing = 0;
                    //
                    ctx->dec.dst.msWithoutEnqueuing = 0;
                    ctx->dec.dst.msWithoutDequeuing = 0;
                    //
                    ctx->dec.msWithoutFeedFrame = 0;
                }
            }
        }
    }
    //
    ctx->dec.msOpen += ms;
    ctx->dec.msWithoutFeedFrame += ms;
}

void StreamContext_cnsmRespHttpHeader_(STStreamContext* ctx){
    while(!ctx->net.resp.headerEnded && ctx->net.buff.buffCsmd < ctx->net.buff.buffUse){
        //0 = [], 1 = ['\r'], 2 = ['\r', '\n'], 3 = ['\r', '\n', '\r']
        unsigned char c = ctx->net.buff.buff[ctx->net.buff.buffCsmd];
        switch(ctx->net.resp.headerEndSeq){
            case 0:
                if(c == '\r') ctx->net.resp.headerEndSeq++;
                break;
            case 1:
                if(c == '\n') ctx->net.resp.headerEndSeq++;
                else if(c == '\r') ctx->net.resp.headerEndSeq = 1;
                else ctx->net.resp.headerEndSeq = 0;
                break;
            case 2:
                if(c == '\r') ctx->net.resp.headerEndSeq++;
                else ctx->net.resp.headerEndSeq = 0;
                break;
            case 3:
                if(c == '\n') {
                    ctx->net.resp.headerEndSeq = 0;
                    ctx->net.resp.headerEnded = 1;
                    printf("Net, response body started (after %d bytes header).\n", (ctx->net.resp.headerSz + 1));
                    //print
                    {
                        if((ctx->net.buff.buffCsmd + 1) < ctx->net.buff.buffSz){
                            unsigned char b = ctx->net.buff.buff[ctx->net.buff.buffCsmd + 1];
                            ctx->net.buff.buff[ctx->net.buff.buffCsmd + 1] = '\0';
                            //printf("Net, response header (last-read):\n-->%s<--.\n", &ctx->net.buff.buff[0]);
                            ctx->net.buff.buff[ctx->net.buff.buffCsmd + 1] = b;
                        }
                    }
                } else if(c == '\r'){
                    ctx->net.resp.headerEndSeq = 1;
                } else {
                    ctx->net.resp.headerEndSeq = 0;
                }
                break;
            default:
                //program-logic error
                break;
                
        }
        ctx->net.resp.headerSz++;
        ctx->net.buff.buffCsmd++;
    }
}

int StreamContext_getPollEventsMask(STStreamContext* ctx){
    int events = POLLERR | POLLPRI;
    //src-events only if dst-is-runnning
    /*
    if(ctx->dec.src.isExplicitON && ctx->dec.src.isImplicitON && ctx->dec.dst.isExplicitON && ctx->dec.dst.isImplicitON){
        if(0 == VideoFrames_peekFrameForRead(&ctx->frames.filled)){
            events |= POLLOUT | POLLWRNORM; //src
        }
        events |= POLLIN | POLLRDNORM; //dst
    }
    */
    //src-events and dst-events indepently
    if(ctx->dec.src.isExplicitON && ctx->dec.src.isImplicitON){
        if(0 == VideoFrames_peekFrameForRead(&ctx->frames.filled)){
            events |= POLLOUT | POLLWRNORM; //src
        }
        if(ctx->dec.dst.isExplicitON && ctx->dec.dst.isImplicitON){
            events |= POLLIN | POLLRDNORM; //dst
        }
    }
    return events;
}

void StreamContext_updatePollMask_(STStreamContext* ctx, struct STPlayer_* plyr){
    int events = StreamContext_getPollEventsMask(ctx), eventsBefore = 0;
    if(0 != Player_pollUpdate(plyr, ENPlayerPollFdType_Decoder, ctx, ctx->dec.fd, events, &eventsBefore)){ //read
        printf("ERROR, poll-update-failed to '%s'.\n", ctx->cfg.path);
    } else {
        if(eventsBefore != events){
#           ifdef K_DEBUG_VERBOSE
            /*if((eventsBefore & (POLLERR)) != (events & (POLLERR))){
                printf("StreamContext, poll-mask: %cerr.\n", (eventsBefore & (POLLERR)) ? '-' : '+');
            }
            if((eventsBefore & (POLLPRI)) != (events & (POLLPRI))){
                printf("StreamContext, poll-mask: %cevents.\n", (eventsBefore & (POLLPRI)) ? '-' : '+');
            }
            if((eventsBefore & (POLLOUT | POLLWRNORM)) != (events & (POLLOUT | POLLWRNORM))){
                printf("StreamContext, poll-mask: %csrc.\n", (eventsBefore & (POLLOUT | POLLWRNORM)) ? '-' : '+');
            }
            if((eventsBefore & (POLLIN | POLLRDNORM)) != (events & (POLLIN | POLLRDNORM))){
                printf("StreamContext, poll-mask: %cdst.\n", (eventsBefore & (POLLIN | POLLRDNORM)) ? '-' : '+');
            }*/
#           endif
            //printf("StreamContext, device-poll listening:%s%s%s%s%s.\n", (events & POLLERR) ? " errors" : "", (events & POLLPRI) ? " events": "", (events & (POLLOUT | POLLWRNORM)) ? " src": "", (events & (POLLIN | POLLRDNORM)) ? " dst": "", (events & (POLLERR | POLLPRI | POLLOUT | POLLWRNORM | POLLIN | POLLRDNORM)) == 0 ? "none" : "");
        }
    }
}

void StreamContext_cnsmFrameOportunity_(STStreamContext* ctx, struct STPlayer_* plyr){
    if(ctx->dec.fd >= 0 && 0 == VideoFrames_peekFrameForRead(&ctx->frames.filled)){
        STBuffer* buff = NULL;
        int buffIsDequeued = 0;
        //try to use a new buffer
        if(buff == NULL){
            if(0 != Buffers_getUnqueued(&ctx->dec.src, &buff, NULL)){
                buff = NULL;
            }
        }
        //try to dequeue and reuse bufer
        if(buff == NULL && ctx->dec.src.enqueuedCount > 0){
            if(0 != Buffers_dequeue(&ctx->dec.src, ctx->dec.fd, &buff, NULL)){
                buff = NULL;
            } else {
                buffIsDequeued = 1;
            }
        }
        //feed frame
        if(buff != NULL){
            STVideoFrame* frame = NULL;
            if(0 != VideoFrames_pullFrameForRead(&ctx->frames.filled, &frame)){
                //nothing
            } else {
                //validate
                if(buff->planesSz < 0){
                    printf("ERROR, StreamContext, at least one plane is required.\n");
                } else if(buff->planes[0].length < frame->buff.use){
                    printf("ERROR, StreamContext, frame doesnt fit on plane's buffer.\n");
                } else {
                    if(ctx->dec.isWaitingForIDRFrame && !frame->state.isIndependent){
#                       ifdef K_DEBUG_VERBOSE
                        printf("StreamContext, frame(#%d) ignored, waiting-for-IDR, %d states-fed.\n", (frame->state.iSeq + 1), ctx->dec.frames.fed.use);
#                       endif
                    } else {
                        struct timeval vTimestamp; //virtual timestamp
                        memset(&vTimestamp, 0, sizeof(vTimestamp));
                        ctx->dec.isWaitingForIDRFrame = 0;
                        //sync record
                        {
                            int i;
                            //first plane
                            STPlane* p = &buff->planes[0];
                            memcpy(p->dataPtr, frame->buff.ptr, frame->buff.use);
                            p->used = frame->buff.use;
                            //others
                            for(i = 1; i < buff->planesSz; i++){
                                STPlane* p = &buff->planes[i];
                                p->used = 0;
                            }
                            //virtual timestamp
                            VideoFrameState_iSeqToTimestamp(frame->state.iSeq, &vTimestamp);
                            //processing timestamps
                            gettimeofday(&frame->state.times.proc.start, NULL);
                            frame->state.times.proc.end = frame->state.times.proc.start;
                        }
                        //queue record
                        if(0 != Buffers_enqueue(&ctx->dec.src, ctx->dec.fd, buff, &vTimestamp)){
                            printf("ERROR, StreamContext, frame could not be queued.\n");
                        } else {
                            //Add frame state to processing queue
                            VideoFrameStates_addNewestCloning(&ctx->dec.frames.fed, &frame->state);
                            ctx->dec.msWithoutFeedFrame = 0;
#                           ifdef K_DEBUG_VERBOSE
                            printf("StreamContext, frame(#%d) queued to src-buffs (%s), %d states-fed.\n", (frame->state.iSeq + 1), (buffIsDequeued ? "dequeued" : "unused"), ctx->dec.frames.fed.use);
#                           endif
                        }
                    }
                }
                //reuse
                if(0 != VideoFrames_pushFrameOwning(&ctx->frames.reusable, frame)){
                    printf("ERROR, StreamContext, frame could not be returned to reusable.\n");
                } else {
                    frame = NULL; //consume
                }
                //release (if not consumed)
                if(frame != NULL){
                    free(frame);
                    frame = NULL;
                }
                //update poll (last filled-frame was consumed)
                if(0 != VideoFrames_peekFrameForRead(&ctx->frames.filled)){
                    StreamContext_updatePollMask_(ctx, plyr);
                }
            }
        }
    }
}
    
void StreamContext_cnsmRespNAL_(STStreamContext* ctx, struct STPlayer_* plyr){
    const unsigned char* bStart = (const unsigned char*)&ctx->net.buff.buff[ctx->net.buff.buffCsmd];
    const unsigned char* bAfterEnd = (const unsigned char*)&ctx->net.buff.buff[ctx->net.buff.buffUse];
    const unsigned char* b = bStart;
    const unsigned char* bChunkStart = bStart;
    unsigned char hdr[4] = { 0x00, 0x00, 0x00, 0x01 };
    //
    while(b < bAfterEnd){
        if(*b == 0x00){
            ctx->net.resp.nal.zeroesSeqAccum++;
        } else {
            //analyze end-of-header
            if(*b == 0x01 && ctx->net.resp.nal.zeroesSeqAccum >= 3){
                if(ctx->frames.filling != NULL){
                    //copy tail data of previous NAL
                    {
                        const int curChunkSz = (const int)((b + 1) - bChunkStart); //(b + 1) because we are still in last-byte-of-header
                        if(curChunkSz > 4){ //all, except current 4-bytes header (0x00, 0x00, 0x00, 0x01)
                            if(0 != VideoFrame_copy(ctx->frames.filling, bChunkStart, curChunkSz - 4)){
                                printf("ERROR, VideoFrame_copy failed.\n");
                                VideoFrame_release(ctx->frames.filling);
                                free(ctx->frames.filling);
                                ctx->frames.filling = NULL;
                                ctx->frames.fillingNalSz = 0;
                            } else {
                                ctx->frames.fillingNalSz += (curChunkSz - 4);
                            }
                            //move chunk start
                            bChunkStart += (curChunkSz - 4);
                        }
                    }
                    //analyze previous NAL type
                    if(ctx->frames.filling->buff.use < ctx->frames.fillingNalSz){
                        printf("ERROR program-logic: (ctx->frames.filling->buff.use < ctx->frames.fillingNalSz).\n");
                    } else if(ctx->frames.fillingNalSz < 5){
                        printf("ERROR zero-size-NAL.\n");
                    } else {
                        const unsigned char lastNalFirstByte = ctx->frames.filling->buff.ptr[ctx->frames.filling->buff.use - ctx->frames.fillingNalSz + 4];
                        const unsigned char lastNalType = (lastNalFirstByte & 0x1F);
                        //printf("StreamContext, nal-type(%d, %d bytes) completed frame(%d bytes).\n", lastNalType, ctx->frames.fillingNalSz, ctx->frames.filling->buff.use);
                        if(lastNalType == 5 || lastNalType == 1){
                            //start next frame
                            const int curChunkSz = (const int)((b + 1) - bChunkStart); //(b + 1) because we are still in last-byte-of-header
                            if(curChunkSz < 4){
                                //some of the 4-bytes header were added to the previous frame
                                const int bToRemove = (4 - curChunkSz);
                                if(ctx->frames.filling->buff.use < bToRemove){
                                    printf("ERROR program-logic: (ctx->frames.filling->buff.use < bToRemove).\n");
                                } else {
                                    printf("StreamContext, removed %d bytes from previous frame (belonged to next frame).\n", bToRemove);
                                    ctx->frames.filling->buff.use -= bToRemove;
                                }
                            }
                            //consume frame
                            long msToArrive = 0;
                            {
                                if(lastNalType == 5){
                                    ctx->frames.filling->state.isIndependent = 1;
                                } else {
                                    ctx->frames.filling->state.isIndependent = 0;
                                }
                                gettimeofday(&ctx->frames.filling->state.times.arrival.end, NULL);
                                msToArrive = msBetweenTimevals(&ctx->frames.filling->state.times.arrival.start, &ctx->frames.filling->state.times.arrival.end);
                            }
                            //flush queue (if independent frame arrived)
                            if(ctx->frames.filling->state.isIndependent){
                                int skippedCount = 0;
                                STVideoFrame* frame = NULL;
                                while(0 == VideoFrames_pullFrameForRead(&ctx->frames.filled, &frame)){
                                    skippedCount++;
                                    //reuse
                                    if(0 != VideoFrames_pushFrameOwning(&ctx->frames.reusable, frame)){
                                        printf("ERROR, StreamContext, frame could not be returned to reusable.\n");
                                    } else {
                                        frame = NULL; //consume
                                    }
                                    //release (if not consumed)
                                    if(frame != NULL){
                                        free(frame);
                                        frame = NULL;
                                    }
                                }
                                //
                                if(skippedCount > 0){
                                    printf("StreamContext, %d frames skipped (independent frame arrived).\n", skippedCount);
                                }
                            }
                            //
                            const int filledWasEmpty = (0 != VideoFrames_peekFrameForRead(&ctx->frames.filled) ? 1 : 0);
                            if(0 != VideoFrames_pushFrameOwning(&ctx->frames.filled, ctx->frames.filling)){
                                printf("ERROR VideoFrames_pushFrameOwning failed.\n");
                                VideoFrame_release(ctx->frames.filling);
                                free(ctx->frames.filling);
                                ctx->frames.filling = NULL;
                                ctx->frames.fillingNalSz = 0;
                            } else {
#                               ifdef K_DEBUG_VERBOSE
                                printf("StreamContext, Frame(#%d, %d bytes, %dms) received (%d filled-frames in queue).\n", (ctx->frames.filling->state.iSeq + 1), ctx->frames.filling->buff.use, msToArrive, ctx->frames.filled.use);
#                               endif
                                ctx->frames.filling = NULL;
                                ctx->frames.fillingNalSz = 0;
                                //decoder
                                if(ctx->dec.fd >= 0){
                                    //auto-start at first NAL arrival
                                    if(!ctx->dec.src.isExplicitON){
                                        if(0 != Buffers_start(&ctx->dec.src, ctx->dec.fd)){
                                            printf("ERROR, StreamContext, Buffers_start failed to '%s'.\n", ctx->cfg.device);
                                        } else {
                                            printf("StreamContext, src-started '%s'.\n", ctx->cfg.device);
                                        }
                                    }
                                    //feed (if running and buffers are not queued yet)
                                    if(ctx->dec.src.isExplicitON && ctx->dec.src.isImplicitON && ctx->dec.src.enqueuedCount < ctx->dec.src.use){
                                        StreamContext_cnsmFrameOportunity_(ctx, plyr);
                                    }
                                    //update poll (first filled-frame is available)
                                    if(filledWasEmpty){
                                        StreamContext_updatePollMask_(ctx, plyr);
                                    }
                                }
                            }
                        } else {
                            //start NAL and continue frame
                            if(0 != VideoFrame_copy(ctx->frames.filling, hdr, sizeof(hdr))){
                                printf("ERROR, VideoFrame_copy failed.\n");
                                VideoFrame_release(ctx->frames.filling);
                                free(ctx->frames.filling);
                                ctx->frames.filling = NULL;
                                ctx->frames.fillingNalSz = 0;
                            } else {
                                ctx->frames.fillingNalSz = sizeof(hdr);
                            }
                            //bChunkStart = b + 1; //redundant with 'start-of-next-chunk' below
                        }
                    }
                }
                //start frame
                if(ctx->frames.filling == NULL){
                    //create chunk
                    if(0 != VideoFrames_pullFrameForFill(&ctx->frames.reusable, &ctx->frames.filling)){
                        printf("StreamContext, VideoFrames_pullFrameForFill failed.\n");
                        ctx->frames.filling = NULL;
                    } else {
                        //copy 4 header bytes
                        if(0 != VideoFrame_copy(ctx->frames.filling, hdr, sizeof(hdr))){
                            printf("ERROR, VideoFrame_copy failed.\n");
                            VideoFrame_release(ctx->frames.filling);
                            free(ctx->frames.filling);
                            ctx->frames.filling = NULL;
                            ctx->frames.fillingNalSz = 0;
                        } else {
                            ctx->frames.fillingNalSz = sizeof(hdr);
                            //initial state
                            //ctx->frames.filling->state.iSeq
                            gettimeofday(&ctx->frames.filling->state.times.arrival.start, NULL);
                            gettimeofday(&ctx->frames.filling->state.times.arrival.end, NULL);
                            gettimeofday(&ctx->frames.filling->state.times.proc.start, NULL);
                            gettimeofday(&ctx->frames.filling->state.times.proc.end, NULL);
                        }
                        //bChunkStart = b + 1; //redundant with 'start-of-next-chunk' below
                    }
                }
                //start-of-next-chunk
                bChunkStart = b + 1;
            }
            //reset zeroes-zeq
            ctx->net.resp.nal.zeroesSeqAccum = 0;
        }
        b++;
    }
    //copy last (unconsumed) chunk to current frame
    if(bChunkStart < bAfterEnd){
        if(ctx->frames.filling == NULL){
            printf("ERROR, program-logic: last data chunk without frame to write.\n");
        } else {
            const int sztoCpy = (int)(bAfterEnd - bChunkStart);
            if(0 != VideoFrame_copy(ctx->frames.filling, bChunkStart, sztoCpy)){
                printf("ERROR, VideoFrame_copy failed.\n");
                VideoFrame_release(ctx->frames.filling);
                free(ctx->frames.filling);
                ctx->frames.filling = NULL;
                ctx->frames.fillingNalSz = 0;
            } else {
                ctx->frames.fillingNalSz += sztoCpy;
            }
            //start-of-next-chunk
            bChunkStart = bAfterEnd;
        }
    }
}

void StreamContext_pollCallbackDevice_(STStreamContext* ctx, struct STPlayer_* plyr, int revents){
    //printf("Device, poll-event(%d):%s%s%s%s%s%s.\n", revents, (revents & POLLERR) ? " errors" : "", (revents & POLLPRI) ? " events": "", (revents & (POLLOUT | POLLWRNORM)) ? " src-hungry": "", (revents & (POLLIN | POLLRDNORM)) ? " dst-populated": "", (revents & (POLLERR | POLLPRI | POLLOUT | POLLWRNORM | POLLIN | POLLRDNORM)) == 0 ? "none" : "", (revents & ~(POLLERR | POLLPRI | POLLOUT | POLLWRNORM | POLLIN | POLLRDNORM)) != 0 ? "others" : "");
    //error
    if((revents & (POLLERR))){
        //printf("Device, poll-event: error.\n");
    }
    //event
    if((revents & (POLLPRI))){
        //printf("Device, poll-event: event.\n");
        int rr = 0, knownPends = 1, eventsDequeuedTotal = 0;
        int isResolutionChangeEvent = 0;
        while(rr == 0 && knownPends > 0){
            struct v4l2_event ev;
            memset(&ev, 0, sizeof(ev));
            //
            rr = v4l2_ioctl(ctx->dec.fd, VIDIOC_DQEVENT, &ev);
            if(rr != 0){
                if(errno == EAGAIN){
                    //no pending events
                    knownPends = 0;
                    rr = 0;
                } else {
                    //error
                    printf("StreamContext, VIDIOC_DQEVENT returned errno(%d).\n", errno);
                    knownPends = 0;
                    rr = 0;
                }
            } else {
                //analyze event
                switch(ev.type){
                    //V4L2_EVENT_RESOLUTION_CHANGE == V4L2_EVENT_SOURCE_CHANGE
#                   ifdef V4L2_EVENT_RESOLUTION_CHANGE
                    case V4L2_EVENT_RESOLUTION_CHANGE:
                        {
                            printf("StreamContext, event(V4L2_EVENT_RESOLUTION_CHANGE).\n");
                            isResolutionChangeEvent = 1;
                        }
                        break;
#                   else
                    case V4L2_EVENT_SOURCE_CHANGE:
                        /*
                        https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/dev-decoder.html
                        Means:
                        - coded resolution (OUTPUT width and height),
                        - visible resolution (selection rectangles),
                        - the minimum number of buffers needed for decoding,
                        - bit-depth of the bitstream has been changed.
                        */
                        {
                            //specifies a flag for resolution changes
                            struct v4l2_event_src_change* src_change = &ev.u.src_change;
                            isResolutionChangeEvent = 1;
                            {
                                printf("StreamContext, event(V4L2_EVENT_SOURCE_CHANGE).\n");
                            }
                            //
                            if((src_change->changes & V4L2_EVENT_SRC_CH_RESOLUTION)){
                                printf("                 change: CH_RESOLUTION.\n");
                                src_change->changes &= ~V4L2_EVENT_SRC_CH_RESOLUTION;
                            }
                            if(src_change->changes){
                                printf("                 change: UNKNOWN(%u).\n", src_change->changes);
                            }
                        }
                        break;
#                   endif
                    case V4L2_EVENT_EOS: //last frame has been decoded
                        printf("StreamContext, event(V4L2_EVENT_EOS).\n");
                        break;
                    default:
                        printf("StreamContext, event(%d, UNSUPPORTED BY THIS CODE).\n", ev.type);
                        break;
                }
                //
                if(ev.pending > 0){
                    printf("StreamContext, %d pending after this one.\n", ev.pending);
                }
                //apply resolution change inmediatly
                if(isResolutionChangeEvent){
                    ctx->dec.dst.isImplicitON = 0; //resolution-change events internally stops the dst-side of the devie
                    //remove current buffers
                    if(ctx->dec.dst.isExplicitON){
                        if(0 != StreamContext_stopAndUnmapBuffs(ctx, &ctx->dec.dst)){
                            printf("WARNING, StreamContext_stopAndUnmapBuffs(dst) failed: '%s'.\n", ctx->cfg.device);
                        } else {
                            printf("StreamContext, dst uninited: '%s'.\n", ctx->cfg.device);
                        }
                    }
                    //create new buffers
                    if(0 != StreamContext_initAndStartDst(ctx, plyr)){
                        printf("ERROR, StreamContext_initAndStartDst(dst) failed: '%s'.\n", ctx->cfg.device);
                    } else {
                        printf("StreamContext, dst inited and started: '%s'.\n", ctx->cfg.device);
                    }
                }
                knownPends = ev.pending;
            }
        }
    }
    //src is ready for new input
    if((revents & (POLLOUT | POLLWRNORM))){
        //printf("Device, poll-event: src-hungry.\n");
        StreamContext_cnsmFrameOportunity_(ctx, plyr);
    }
    //dst has new output ready
    if((revents & (POLLIN | POLLRDNORM))){
        //printf("Device, poll-event: dst-populated.\n");
        STBuffer* buff = NULL;
        struct timeval timestamp;
        memset(&timestamp, 0, sizeof(timestamp));
        if(0 != Buffers_dequeue(&ctx->dec.dst, ctx->dec.fd, &buff, &timestamp)){
            buff = NULL;
        } else {
            int framesSkippedByDecoderCount = 0;
            unsigned long frameSeqIdx = 0; STVideoFrameState frameState;
            VideoFrameState_init(&frameState);
            VideoFrameState_timestampToSeqIdx(&timestamp, &frameSeqIdx);
            VideoFrameStates_getStateCloningAndRemoveOlder(&ctx->dec.frames.fed, frameSeqIdx, &frameState, &framesSkippedByDecoderCount);
            if(frameState.iSeq == frameSeqIdx){
                gettimeofday(&frameState.times.proc.end, NULL);
                long msToArrive = msBetweenTimevals(&frameState.times.proc.start, &frameState.times.proc.end);
#               ifdef K_DEBUG_VERBOSE
                printf("StreamContext, frame(#%d) output obtained (%dms inside device).\n", (frameSeqIdx + 1), msToArrive);
#               endif
            } else {
#               ifdef K_DEBUG_VERBOSE
                printf("StreamContext, frame(#%d) output obtained (no state-fed found).\n", (frameSeqIdx + 1));
#               endif
            }
            if(framesSkippedByDecoderCount > 0){
                printf("WARNING, StreamContext, decoder skipped %d frames fed.\n");
            }
            //invalidate framebuffers where this frame will be used
            {
                int i; for(i = 0; i < plyr->fbs.arrUse; i++){
                    STFramebuff* fb = plyr->fbs.arr[i];
                    if(fb->pixFmt == ctx->dec.dst.pixelformat){
                        if(0 == Framebuff_streamFindStreamId(fb, ctx->streamId)){
                            //flag as dirty
                            fb->offscreen.isSynced = 0;
                        }
                    }
                }
            }
            //enqueue buffers
            while(ctx->dec.dst.enqueuedCount < ctx->dec.dst.enqueuedRequiredMin){
                STBuffer* buffFnd = NULL;
                //find another available buffer to queue, to avoid cloning current buffer.
                if(0 != Buffers_getUnqueued(&ctx->dec.dst, &buffFnd, buff)){
                    buffFnd = NULL;
                } else {
                    //(other) available buffer found
                    //sync record (empty)
                    {
                        //others
                        int i; for(i = 0; i < buffFnd->planesSz; i++){
                            STPlane* p = &buffFnd->planes[i];
                            p->used = 0;
                        }
                    }
                    if(0 != Buffers_enqueue(&ctx->dec.dst, ctx->dec.fd, buffFnd, NULL)){
                        printf("ERROR, StreamContext, dst-buff could not be queued.\n");
                        buffFnd = NULL;
                    }
                }
                //add current bufer if necesary
                if(buffFnd == NULL && buff != NULL){
                    //keep last as a clone
                    if(0 != Buffers_keepLastAsClone(&ctx->dec.dst, buff)){
                        printf("ERROR, StreamContext, frame(#%d) could not be cloned to 'last'.\n", (frameSeqIdx + 1));
                    }
                    buffFnd = buff; buff = NULL;
                    //sync record (empty)
                    {
                        //others
                        int i; for(i = 0; i < buffFnd->planesSz; i++){
                            STPlane* p = &buffFnd->planes[i];
                            p->used = 0;
                        }
                    }
                    if(0 != Buffers_enqueue(&ctx->dec.dst, ctx->dec.fd, buffFnd, NULL)){
                        printf("ERROR, StreamContext, dst-buff could not be queued.\n");
                        buffFnd = NULL;
                    }
                }
                //stop if no activity
                if(buffFnd == NULL){
                    break;
                }
            }
            VideoFrameState_release(&frameState);
        }
    }
}

void StreamContext_pollCallbackSocket_(STStreamContext* ctx, struct STPlayer_* plyr, int revents){
    //error
    if((revents & (POLLERR))){
        printf("ERROR, StreamContext, poll-err-flag active at socket '%s:%d'.\n", ctx->cfg.server, ctx->cfg.port);
        Player_pollAutoRemove(plyr, ENPlayerPollFdType_SrcSocket, ctx, ctx->net.socket);
        close(ctx->net.socket);
        ctx->net.socket = 0;
    } else {
        //write
        if((revents & POLLOUT)){
            //reset
            ctx->net.msWithoutSend = 0;
            //send
            if(ctx->net.req.payCsmd < ctx->net.req.payUse){
                const int sent = (int)send(ctx->net.socket, &ctx->net.req.pay[ctx->net.req.payCsmd], (ctx->net.req.payUse - ctx->net.req.payCsmd), 0);
                if(sent > 0){
                    ctx->net.req.payCsmd += sent;
                    if(ctx->net.req.payUse == ctx->net.req.payCsmd){
                        printf("StreamContext, request sent (%d bytes) to '%s:%d'.\n", ctx->net.req.payCsmd, ctx->cfg.server, ctx->cfg.port);
                        //printf("StreamContext, -->\n%s\n<--\n", ctx->net.req.pay);
                        //stop writting, start reading-only
                        if(0 != Player_pollUpdate(plyr, ENPlayerPollFdType_SrcSocket, ctx, ctx->net.socket, POLLIN, NULL)){ //read
                            printf("ERROR, StreamContext, poll-update-failed to '%s:%d'.\n", ctx->cfg.server, ctx->cfg.port);
                            Player_pollAutoRemove(plyr, ENPlayerPollFdType_SrcSocket, ctx, ctx->net.socket);
                            close(ctx->net.socket);
                            ctx->net.socket = 0;
                        }
                    }
                } else if(sent != 0){ //zero = socket propperly shuteddown
                    if(errno == EAGAIN || errno == EWOULDBLOCK){
                        //non-blocking
                    } else {
                        printf("ERROR, StreamContext, send failed to '%s:%d'.\n", ctx->cfg.server, ctx->cfg.port);
                        Player_pollAutoRemove(plyr, ENPlayerPollFdType_SrcSocket, ctx, ctx->net.socket);
                        close(ctx->net.socket);
                        ctx->net.socket = 0;
                    }
                }
            }
        }
        //read
        if((revents & POLLIN)){
            //reset
            ctx->net.msWithoutRecv = 0;
            //recv
            //empty buffer if fully consumed (lock is not necesary)
            if(ctx->net.buff.buffCsmd >= ctx->net.buff.buffUse){
                ctx->net.buff.buffCsmd = 0;
                ctx->net.buff.buffUse = 0;
            }
            //produce
            if(ctx->net.buff.buffUse >=  ctx->net.buff.buffSz){
                //buffer is full, wait for consumer
                int mSecs = 10;
                usleep((useconds_t)mSecs * 1000);
            } else {
                //produce (unlocked)
                int rcvd = (int)recv(ctx->net.socket, &ctx->net.buff.buff[ctx->net.buff.buffUse], (ctx->net.buff.buffSz - ctx->net.buff.buffUse), 0);
                if(rcvd > 0){
                    //printf("Net, %d/%d revd.\n", rcvd, (ctx->net.buff.buffSz - ctx->net.buff.buffUse));
                    ctx->net.buff.buffUse += rcvd;
                    //read header
                    if(!ctx->net.resp.headerEnded){
                        StreamContext_cnsmRespHttpHeader_(ctx);
                    }
                    //read body (NALs)
                    if(ctx->net.resp.headerEnded){
                        StreamContext_cnsmRespNAL_(ctx, plyr);
                    }
                    //mark as fully consumed
                    ctx->net.buff.buffCsmd = ctx->net.buff.buffUse;
                } else if(rcvd != 0){ //zero = socket propperly shuteddown
                    if(errno == EAGAIN || errno == EWOULDBLOCK){
                        //non-blocking
                    } else {
                        printf("ERROR, StreamContext, recv failed to '%s:%d'.\n", ctx->cfg.server, ctx->cfg.port);
                        Player_pollAutoRemove(plyr, ENPlayerPollFdType_SrcSocket, ctx, ctx->net.socket);
                        close(ctx->net.socket);
                        ctx->net.socket = 0;
                    }
                }
            }
        }
    }
}
    
void StreamContext_pollCallback(void* userParam, struct STPlayer_* plyr, const ENPlayerPollFdType type, int revents){
    STStreamContext* ctx = (STStreamContext*)userParam;
    switch(type){
        case ENPlayerPollFdType_Decoder: //dec (decoder).fd
            StreamContext_pollCallbackDevice_(ctx, plyr, revents);
            break;
        case ENPlayerPollFdType_SrcSocket: //net.socket
            StreamContext_pollCallbackSocket_(ctx, plyr, revents);
            break;
        default:
            break;
    }
}

int StreamContext_open(STStreamContext* ctx, struct STPlayer_* plyr, const char* device, const char* server, const unsigned int port, const char* resPath, int srcPixFmt /*V4L2_PIX_FMT_H264*/, int buffersAmmount, int planesPerBuffer, int sizePerPlane, int dstPixFmt /*V4L2_PIX_FMT_*/, const int connTimeoutSecs, const int decoderTimeoutSecs){
    int r = -1;
    if(device == NULL || device[0] == '\0'){
        printf("ERROR, StreamContext_open, device-param is empty.\n");
    } else if(server == NULL || server[0] == '\0'){
        printf("ERROR, StreamContext_open, server-param is empty.\n");
    } else if(resPath == NULL || resPath[0] == '\0'){
        printf("ERROR, StreamContext_open, resPath-param is empty.\n");
    } else if(port <= 0){
        printf("ERROR, StreamContext_open, port-param is empty.\n");
#   ifdef K_USE_MPLANE
    } else if(0 != Buffers_setNameAndType(&ctx->dec.src, "src", V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)){
        printf("ERROR, StreamContext_open, Buffers_setNameAndType failed.\n");
#   else
    } else if(0 != Buffers_setNameAndType(&ctx->dec.src, "src", V4L2_BUF_TYPE_VIDEO_OUTPUT)){
        printf("ERROR, StreamContext_open, Buffers_setNameAndType failed.\n");
#   endif
#   ifdef K_USE_MPLANE
    } else if(0 != Buffers_setNameAndType(&ctx->dec.dst, "dst", V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)){
        printf("ERROR, StreamContext_open, Buffers_setNameAndType failed.\n");
#   else
    } else if(0 != Buffers_setNameAndType(&ctx->dec.dst, "dst", V4L2_BUF_TYPE_VIDEO_CAPTURE)){
        printf("ERROR, StreamContext_open, Buffers_setNameAndType failed.\n");
#   endif
    } else {
        printf("StreamContext_open, opening device: '%s'...\n", resPath);
        int fd = v4l2_open(device, O_RDWR | O_NONBLOCK);
        if(fd < 0){
            printf("ERROR, StreamContext_open, device failed to open: '%s'.\n", resPath);
        } else {
            int srcPixFmtWasFound = 0, dstPixFmtWasFound = 0;
            const char* dstPixFmtChars = (const char*)&dstPixFmt;
            //
            STPrintedInfo* printDev = Player_getPrintIfNotRecent(plyr, device, 0, 0, K_DEF_REPRINTS_HIDE_SECS);
            STPrintedInfo* printSrcFmt = Player_getPrintIfNotRecent(plyr, device, srcPixFmt, 0, K_DEF_REPRINTS_HIDE_SECS);
            //
            if(printDev != NULL){ PrintedInfo_touch(printDev); }
            if(printSrcFmt != NULL){ PrintedInfo_touch(printSrcFmt); }
            //
            if(0 != v4lDevice_queryCaps(fd, (printDev != NULL ? 1 : 0))){
                //device is not v4l?
                printf("ERROR, v4lDevice_queryCaps failed: '%s'.\n", resPath);
            } else if(0 != Buffers_queryFmts(&ctx->dec.src, fd, srcPixFmt, &srcPixFmtWasFound, (printSrcFmt != NULL ? 1 : 0))){
                printf("ERROR, Buffers_queryFmts(src) failed: '%s'.\n", resPath);
            } else if(!srcPixFmtWasFound){
                printf("ERROR, Buffers_queryFmts src-fmt unsupported: '%s'.\n", resPath);
            } else if(0 != Buffers_setFmt(&ctx->dec.src, fd, srcPixFmt, planesPerBuffer, sizePerPlane, (printSrcFmt != NULL ? 1 : 0))){
                printf("ERROR, Buffers_setFmt failed: '%s'.\n", resPath);
            } else if(0 != Buffers_queryFmts(&ctx->dec.dst, fd, dstPixFmt, &dstPixFmtWasFound, (printSrcFmt != NULL ? 1 : 0))){
                printf("ERROR, Buffers_queryFmts(dst) failed: '%s'.\n", resPath);
            } else if(!dstPixFmtWasFound){
                printf("ERROR, Buffers_queryFmts dst-fmt('%c%c%c%c') unsupported: '%s'.\n", dstPixFmtChars[0], dstPixFmtChars[1], dstPixFmtChars[2], dstPixFmtChars[3], resPath);
            } else if(0 != StreamContext_initAndPrepareSrc(ctx, fd, buffersAmmount, (printSrcFmt != NULL ? 1 : 0))){
                printf("ERROR, StreamContext_initAndPrepareSrc(%d) failed: '%s'.\n", buffersAmmount, resPath);
            } else if(0 != StreamContext_eventsSubscribe(ctx, fd)){
                printf("ERROR, StreamContext_eventsSubscribe failed to '%s'.\n", resPath);
            } else if(0 != Player_pollAdd(plyr, ENPlayerPollFdType_Decoder, StreamContext_pollCallback, ctx, fd, StreamContext_getPollEventsMask(ctx))){ //write
                printf("ERROR, Player_pollAdd poll-add-failed to '%s'.\n", resPath);
            } else {
                //cfg
                {
                    if(device != ctx->cfg.device){
                        if(ctx->cfg.device != NULL){ free(ctx->cfg.device); ctx->cfg.device = NULL; }
                        if(device != NULL){
                            int deviceLen = strlen(device);
                            ctx->cfg.device = malloc(deviceLen + 1);
                            memcpy(ctx->cfg.device, device, deviceLen + 1);
                        }
                    }
                    if(server != ctx->cfg.server){
                        if(ctx->cfg.server != NULL){ free(ctx->cfg.server); ctx->cfg.server = NULL; }
                        if(server != NULL){
                            int serverLen = strlen(server);
                            ctx->cfg.server = malloc(serverLen + 1);
                            memcpy(ctx->cfg.server, server, serverLen + 1);
                        }
                    }
                    if(resPath != ctx->cfg.path){
                        if(ctx->cfg.path != NULL){ free(ctx->cfg.path); ctx->cfg.path = NULL; }
                        if(resPath != NULL){
                            int resPathLen = strlen(resPath);
                            ctx->cfg.path = malloc(resPathLen + 1);
                            memcpy(ctx->cfg.path, resPath, resPathLen + 1);
                        }
                    }
                    //
                    ctx->cfg.port               = port;
                    //
                    ctx->cfg.srcPixFmt          = srcPixFmt; //V4L2_PIX_FMT_H264
                    ctx->cfg.buffersAmmount     = buffersAmmount;
                    ctx->cfg.planesPerBuffer    = planesPerBuffer;
                    ctx->cfg.sizePerPlane       = sizePerPlane;
                    ctx->cfg.dstPixFmt          = dstPixFmt; //V4L2_PIX_FMT_*
                    //
                    ctx->cfg.connTimeoutSecs    = connTimeoutSecs;
                    ctx->cfg.decoderTimeoutSecs = decoderTimeoutSecs;
                }
                //dec (decoder)
                {
                    if(ctx->dec.fd >= 0){
                        v4l2_close(ctx->dec.fd);
                        ctx->dec.fd = -1;
                    }
                    ctx->dec.fd = fd;
                    fd = -1; //consume
                    //reset (just in case)
                    {
                        ctx->dec.msWithoutFeedFrame = 0;
                        ctx->dec.isWaitingForIDRFrame = 1;
                    }
                }
                //success
                r = 0;
            }
            //revert
            if(r != 0){
                if(fd >= 0){
                    if(0 != Player_pollAutoRemove(plyr, ENPlayerPollFdType_Decoder, ctx, fd)){
                        printf("ERROR, StreamContext, Player_pollAutoRemove failed.\n");
                    }
                    if(0 != StreamContext_eventsUnsubscribe(ctx, fd)){
                        printf("WARNING, StreamContext_eventsUnsubscribe failed.\n");
                    }
                }
                if(0 != StreamContext_stopAndUnmapBuffs(ctx, &ctx->dec.src)){
                    printf("WARNING, StreamContext_stopAndUnmapBuffs(src) failed.\n");
                }
            }
            //release (if not consumed)
            if(fd >= 0){
                v4l2_close(fd);
                fd = -1;
            }
        }
    }
    //
    return r;
}

int StreamContext_close(STStreamContext* ctx, struct STPlayer_* plyr){
    if(0 != StreamContext_stopAndUnmapBuffs(ctx, &ctx->dec.dst)){
        printf("WARNING, StreamContext_stopAndUnmapBuffs(dst) failed.\n");
    }
    if(0 != StreamContext_stopAndUnmapBuffs(ctx, &ctx->dec.src)){
        printf("WARNING, StreamContext_stopAndUnmapBuffs(src) failed.\n");
    }
    if(0 != StreamContext_eventsUnsubscribe(ctx, ctx->dec.fd)){
        printf("ERROR, StreamContext, unsubscribe failed.\n");
    }
    if(ctx->dec.fd >= 0){
        if(0 != Player_pollAutoRemove(plyr, ENPlayerPollFdType_Decoder, ctx, ctx->dec.fd)){
            printf("ERROR, StreamContext, Player_pollAutoRemove failed.\n");
        }
        v4l2_close(ctx->dec.fd);
        ctx->dec.fd = -1;
    }
    //reset (just in case)
    {
        ctx->dec.src.isExplicitON       = 0;
        ctx->dec.src.isImplicitON       = 0;
        //
        ctx->dec.dst.isExplicitON       = 0;
        ctx->dec.dst.isImplicitON       = 0;
    }
    return 0;
}

//------------
//-- Plane --
//------------

void Plane_init(STPlane* obj){
    memset(obj, 0, sizeof(*obj));
    obj->fd = -1;
}

void Plane_release(STPlane* obj){
    if(obj->dataPtr != NULL){
        if(!obj->dataPtrIsExternal){
            free(obj->dataPtr);
        }
        obj->dataPtr = NULL;
    }
    if(obj->fd >= 0){
        close(obj->fd);
        obj->fd = -1;
    }
    obj->dataPtrIsExternal = 0;
    obj->used = 0;
    obj->length = 0;
    obj->bytesPerLn = 0;
    obj->memOffset = 0;
}

int Plane_clone(STPlane* obj, const STPlane* src){
    int r = -1;
    //resize plane (if posible)
    if(obj->length != src->length && !obj->dataPtrIsExternal){
        //printf("Plane, clone, resizing length.\n");
        //release
        {
            if(obj->dataPtr != NULL){
                free(obj->dataPtr);
                obj->dataPtr = NULL;
            }
            obj->length = 0;
        }
        //re-allocate
        if(src->length > 0){
            obj->dataPtr = malloc(src->length);
            if(obj->dataPtr != NULL){
                obj->length = src->length;
            }
        }
    }
    //clone
    if(obj == src){
        r = 0;
    } else if(obj->length == src->length){
        obj->used = src->used;
        obj->bytesPerLn = src->bytesPerLn;
        obj->memOffset = src->memOffset;
        if(obj->dataPtr != NULL && src->dataPtr != NULL){
            memcpy(obj->dataPtr, src->dataPtr, src->used);
        }
        r = 0;
    }
    return r;
}

//------------
//-- Buffer --
//------------

void Buffer_init(STBuffer* obj){
    memset(obj, 0, sizeof(*obj));
}

void Buffer_release(STBuffer* obj){
    if(obj->planes != NULL){
        int i; for(i = 0; i < obj->planesSz; i++){
            STPlane* p = &obj->planes[i];
            Plane_release(p);
        }
        free(obj->planes);
        obj->planes = NULL;
    }
    obj->planesSz = 0;
}

int Buffer_clone(STBuffer* obj, const STBuffer* src){
    int r = -1;
    //
    if(obj->planesSz != src->planesSz){
        //printf("Buffer, clone, resizing planes.\n");
        //release
        if(obj->planes != NULL){
            int i; for(i = 0; i < obj->planesSz; i++){
                STPlane* p = &obj->planes[i];
                Plane_release(p);
            }
            free(obj->planes);
            obj->planes = NULL;
        }
        obj->planesSz = 0;
        //reallocate
        if(src->planesSz > 0){
            obj->planes  = malloc(sizeof(STPlane) * src->planesSz);
            if(obj->planes != NULL){
                int j; for(j = 0; j < src->planesSz; j++){
                    STPlane* plane  = &obj->planes[j];
                    Plane_init(plane);
                }
                obj->planesSz = src->planesSz;
            }
        }
    }
    //clone
    if(obj == src){
        r = 0;
    } else if(obj->planesSz == src->planesSz){
        r = 0;
        obj->index      = src->index;
        //dbg
#       ifdef K_DEBUG
        obj->dbg.indexPlusOne = src->dbg.indexPlusOne;
#       endif
        {
            int j; for(j = 0; j < obj->planesSz; j++){
                STPlane* plane  = &obj->planes[j];
                STPlane* planeSrc  = &src->planes[j];
                if(0 != Plane_clone(plane, planeSrc)){
                    r = -1;
                    break;
                }
            }
        }
    }
    return r;
}

//-------------
//-- Buffers --
//-------------

void Buffers_init(STBuffers* obj){
    memset(obj, 0, sizeof(*obj));
    //
    Buffer_init(&obj->lastDequeuedClone);
}

void Buffers_release(STBuffers* obj){
    if(obj->name != NULL){
        free(obj->name);
        obj->name = NULL;
    }
    if(obj->arr != NULL){
        int i; for(i = 0; i < obj->use; i++){
            STBuffer* b = &obj->arr[i];
            Buffer_release(b);
        }
        free(obj->arr);
        obj->arr = NULL;
    }
    obj->use            = 0;
    obj->sz             = 0;
    obj->enqueuedCount  = 0;
    obj->lastDequeued   = NULL;
    obj->isLastDequeuedCloned = 0;
    //
#   ifdef K_USE_MPLANE
    if(obj->srchPlanes != NULL){
        free(obj->srchPlanes);
        obj->srchPlanes = NULL;
    }
#   endif
    Buffer_release(&obj->lastDequeuedClone);
}

int Buffers_setNameAndType(STBuffers* obj, const char* name, const int type){
    int r = -1;
    //name
    {
        if(obj->name != NULL){
            free(obj->name);
            obj->name = NULL;
        }
        if(name != NULL){
            int nameLen = strlen(name);
            obj->name = malloc(nameLen + 1);
            memcpy(obj->name, name, nameLen + 1);
        }
    }
    //type
    {
#       ifdef K_USE_MPLANE
        obj->mp = NULL;
#       else
        obj->sp = NULL;
#       endif
        switch(type){
#           ifdef K_USE_MPLANE
            case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
            case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
                obj->type = type;
                obj->mp = &obj->fm.fmt.pix_mp;
                r = 0;
                break;
#           else
            case V4L2_BUF_TYPE_VIDEO_OUTPUT:
            case V4L2_BUF_TYPE_VIDEO_CAPTURE:
                obj->type = type;
                obj->sp = &obj->fm.fmt.pix;
                r = 0;
                break;
#           endif
            default:
                printf("ERROR, unsupported buffers-type(%d).", type);
                r - 1;
                break;
        }
    }
    return r;
}


int Buffers_queryFmts(STBuffers* obj, int fd, int fmtSearch, int* dstFmtWasFound, const int print){
    int r = -1;
    if(print){
        printf("--------------------------.\n");
        printf("---- QUERING FORMATS  ----.\n");
        printf("---- '%s'.\n", obj->name);
        printf("--------------------------.\n");
    }
    //quering src supported formats
    {
        struct v4l2_fmtdesc fmt;
        memset(&fmt, 0, sizeof(fmt));
        //
        fmt.type = obj->type;
        fmt.index = 0;
        //
        int rr = 0;
        do {
            rr = v4l2_ioctl(fd, VIDIOC_ENUM_FMT, &fmt);
            if(rr == 0){
                const char* pixelformatBytes = (const char*)&fmt.pixelformat;
                if(print){
                    printf("Buffers(%s), coded format #%d: '%c%c%c%c' => '%s'.\n", obj->name, (fmt.index + 1), pixelformatBytes[0], pixelformatBytes[1], pixelformatBytes[2], pixelformatBytes[3], fmt.description);
                }
                //
                if(fmt.pixelformat == fmtSearch){
                    if(dstFmtWasFound != NULL){
                        *dstFmtWasFound = 1;
                    }
                }
                //flags
                if(print){
                    if((fmt.flags & V4L2_FMT_FLAG_COMPRESSED)){ printf("                flag: V4L2_FMT_FLAG_COMPRESSED.\n"); }
                    if((fmt.flags & V4L2_FMT_FLAG_EMULATED)){ printf("                flag: V4L2_FMT_FLAG_EMULATED.\n"); }
                    //V4L2_FMT_FLAG_CONTINUOUS_BYTESTREAM: the engine can receive bytestream, if not, the engine expects one H264 Access Unit per buffer.
#               ifdef V4L2_FMT_FLAG_CONTINUOUS_BYTESTREAM //jetson-nano does not recognizes this
                    if((fmt.flags & V4L2_FMT_FLAG_CONTINUOUS_BYTESTREAM)){ printf("    flag: V4L2_FMT_FLAG_CONTINUOUS_BYTESTREAM.\n"); }
#               endif
#               ifdef V4L2_FMT_FLAG_DYN_RESOLUTION //jetson-nano does not recognizes this
                    if((fmt.flags & V4L2_FMT_FLAG_DYN_RESOLUTION)){ printf("    flag: V4L2_FMT_FLAG_DYN_RESOLUTION.\n"); }
#               endif
#               ifdef V4L2_FMT_FLAG_ENC_CAP_FRAME_INTERVAL //jetson-nano does not recognizes this
                    if((fmt.flags & V4L2_FMT_FLAG_ENC_CAP_FRAME_INTERVAL)){ printf("    flag: V4L2_FMT_FLAG_ENC_CAP_FRAME_INTERVAL.\n"); }
#               endif
#               ifdef V4L2_FMT_FLAG_CSC_COLORSPACE //jetson-nano does not recognizes this
                    if((fmt.flags & V4L2_FMT_FLAG_CSC_COLORSPACE)){ printf("    flag: V4L2_FMT_FLAG_CSC_COLORSPACE.\n"); }
#               endif
#               ifdef V4L2_FMT_FLAG_CSC_XFER_FUNC //jetson-nano does not recognizes this
                    if((fmt.flags & V4L2_FMT_FLAG_CSC_XFER_FUNC)){ printf("    flag: V4L2_FMT_FLAG_CSC_XFER_FUNC.\n"); }
#               endif
#               ifdef V4L2_FMT_FLAG_CSC_YCBCR_ENC //jetson-nano does not recognizes this
                    if((fmt.flags & V4L2_FMT_FLAG_CSC_YCBCR_ENC)){ printf("    flag: V4L2_FMT_FLAG_CSC_YCBCR_ENC.\n"); }
#               endif
#               ifdef V4L2_FMT_FLAG_CSC_HSV_ENC //jetson-nano does not recognizes this
                    if((fmt.flags & V4L2_FMT_FLAG_CSC_HSV_ENC)){ printf("    flag: V4L2_FMT_FLAG_CSC_HSV_ENC.\n"); }
#               endif
#               ifdef V4L2_FMT_FLAG_CSC_QUANTIZATION
                    if((fmt.flags & V4L2_FMT_FLAG_CSC_QUANTIZATION)){ printf("    flag: V4L2_FMT_FLAG_CSC_QUANTIZATION.\n"); }
#               endif
                }
                //frame sizes for this format
                {
                    struct v4l2_frmsizeenum sz;
                    memset(&sz, 0, sizeof(sz));
                    //
                    sz.pixel_format = fmt.pixelformat;
                    sz.index = 0;
                    //
                    int rr2 = 0;
                    do {
                        CALL_IOCTL(rr2, v4l2_ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &sz));
                        if(rr2 == 0){
                            if(print){
                                switch (sz.type) {
                                    case V4L2_FRMSIZE_TYPE_DISCRETE:
                                        printf("                framesize #%d: discrete, width(%u) height(%u).\n", (sz.index + 1), sz.discrete.width, sz.discrete.height);
                                        break;
                                    case V4L2_FRMSIZE_TYPE_CONTINUOUS:
                                        //this is like stepwise, but only one step is defined since steps_sizes are = 1.
                                        printf("                framesize #%d: continuous, width(%u, +%u, %u) height(%u, +%u, %u).\n", (sz.index + 1), sz.stepwise.min_width, sz.stepwise.step_width, sz.stepwise.max_width, sz.stepwise.min_height, sz.stepwise.step_height, sz.stepwise.max_height);
                                        break;
                                    case V4L2_FRMSIZE_TYPE_STEPWISE:
                                        printf("                framesize #%d: stepwise, width(%u, +%u, %u) height(%u, +%u, %u).\n", (sz.index + 1), sz.stepwise.min_width, sz.stepwise.step_width, sz.stepwise.max_width, sz.stepwise.min_height, sz.stepwise.step_height, sz.stepwise.max_height);
                                        break;
                                    default:
                                        printf("                framesize #%d: unknown type.\n", (sz.index + 1));
                                        break;
                                }
                            }
                            //next sz
                            sz.index++;
                        }
                    } while(rr2 != 0);
                }
                //next fmt
                fmt.index++;
            }
        } while(rr == 0);
        //at least one format
        r = (fmt.index > 0 ? 0 : -1);
    }
    return r;
}

int Buffers_setFmt(STBuffers* obj, int fd, int fmt, int planesPerBuffer, int sizePerPlane, const int print){
    int r = -1;
    if(print){
        printf("-------------------------------.\n");
        printf("---- CONFIGURING BUFFERS   ----.\n");
        printf("---- '%s'.\n", obj->name);
        printf("-------------------------------.\n");
    }
    {
        struct v4l2_format* fm = &obj->fm;
        const char* pixelformatBytes = NULL;
        switch(obj->type){
#           ifdef K_USE_MPLANE
            case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
            case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
                pixelformatBytes = (const char*)&obj->mp->pixelformat;
                break;
#           else
            case V4L2_BUF_TYPE_VIDEO_OUTPUT:
            case V4L2_BUF_TYPE_VIDEO_CAPTURE:
                pixelformatBytes = (const char*)&obj->sp->pixelformat;
                break;
#           endif
            default:
                break;
        }
        //
        fm->type = obj->type;
        //read
        {
            int rr; CALL_IOCTL(rr, v4l2_ioctl(fd, VIDIOC_G_FMT, fm));
            if(rr != 0){
                printf("ERROR, Buffers(%s), getting src-format returned(%d).\n", obj->name, rr);
            } else {
                switch(obj->type){
#                   ifdef K_USE_MPLANE
                    case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
                    case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
                        if(print){
                            printf("Buffers(%s), getting pixelformat('%c%c%c%c') width(%u) height(%u) success.\n", obj->name, pixelformatBytes[0], pixelformatBytes[1], pixelformatBytes[2], pixelformatBytes[3], obj->mp->width, obj->mp->height);
                        }
                        obj->pixelformat = obj->mp->pixelformat;
                        obj->width  = obj->mp->width;
                        obj->height = obj->mp->height;
                        {
                            int i; for( i = 0; i < obj->mp->num_planes; i++){
                                struct v4l2_plane_pix_format* pp = &obj->mp->plane_fmt[i];
                                if(print){
                                    printf("    plane #%d, sizeimage(%u) bytesperline(%u).\n", (i + 1), pp->sizeimage, pp->bytesperline);
                                }
                            }
                        }
                        break;
#                   else
                    case V4L2_BUF_TYPE_VIDEO_OUTPUT:
                    case V4L2_BUF_TYPE_VIDEO_CAPTURE:
                        if(print){
                            printf("Buffers(%s), getting pixelformat('%c%c%c%c') width(%u) height(%u) sizeimage(%u) bytesperline(%u) success.\n", obj->name, pixelformatBytes[0], pixelformatBytes[1], pixelformatBytes[2], pixelformatBytes[3], obj->sp->width, obj->sp->height, obj->sp->sizeimage, obj->sp->bytesperline);
                        }
                        obj->pixelformat = obj->sp->pixelformat;
                        obj->width  = obj->sp->width;
                        obj->height = obj->sp->height;
                        break;
#                   endif
                    default:
                        break;
                }
                //read composition rect
                if(0 != Buffers_getCompositionRect(obj, fd, &obj->composition)){
                    if(print){
                        printf("Buffers(%s), getting getCompositionRect returned(%d).\n", obj->name, rr);
                    }
                    obj->composition.x = 0;
                    obj->composition.y = 0;
                    obj->composition.width = obj->width;
                    obj->composition.height = obj->height;
                    if(print){
                        printf("Buffers(%s), implicit composition x(%d, +%d) y(%d, +%d).\n", obj->name, obj->composition.x, obj->composition.width, obj->composition.y, obj->composition.height);
                    }
                } else {
                    if(print){
                        printf("Buffers(%s), explicit composition x(%d, +%d) y(%d, +%d).\n", obj->name, obj->composition.x, obj->composition.width, obj->composition.y, obj->composition.height);
                    }
                }
            }
        }
        //change
        switch(obj->type){
#           ifdef K_USE_MPLANE
            case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
            case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
                obj->mp->pixelformat = fmt;
                obj->mp->num_planes = planesPerBuffer;
                {
                    int i; for(i = 0 ; i < obj->mp->num_planes; i++){
                        obj->mp->plane_fmt[i].sizeimage = sizePerPlane;
                    }
                }
                break;
#           else
            case V4L2_BUF_TYPE_VIDEO_OUTPUT:
            case V4L2_BUF_TYPE_VIDEO_CAPTURE:
                obj->sp->pixelformat = fmt;
                obj->sp->sizeimage = sizePerPlane;
                break;
#           endif
            default:
                break;
        }
        //set
        {
            switch(obj->type){
#               ifdef K_USE_MPLANE
                case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
                case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
                    if(print){
                        printf("Buffers(%s), setting pixelformat('%c%c%c%c') width(%u) height(%u).\n", obj->name, pixelformatBytes[0], pixelformatBytes[1], pixelformatBytes[2], pixelformatBytes[3], obj->mp->width, obj->mp->height);
                    }
                    break;
#               else
                case V4L2_BUF_TYPE_VIDEO_OUTPUT:
                case V4L2_BUF_TYPE_VIDEO_CAPTURE:
                    if(print){
                        printf("Buffers(%s), setting pixelformat('%c%c%c%c') width(%u) height(%u) sizeimage(%u) bytesperline(%u).\n", obj->name, pixelformatBytes[0], pixelformatBytes[1], pixelformatBytes[2], pixelformatBytes[3], obj->sp->width, obj->sp->height, obj->sp->sizeimage, obj->sp->bytesperline);
                    }
                    break;
#               endif
                default:
                    break;
            }
            int rr; CALL_IOCTL(rr, v4l2_ioctl(fd, VIDIOC_S_FMT, fm));
            if(rr != 0){
                switch(obj->type){
#                   ifdef K_USE_MPLANE
                    case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
                    case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
                        if(print){
                            printf("ERROR, Buffers(%s), setting pixelformat('%c%c%c%c') width(%u) height(%u) returnd(%d).\n", obj->name, pixelformatBytes[0], pixelformatBytes[1], pixelformatBytes[2], pixelformatBytes[3], obj->mp->width, obj->mp->height, rr);
                        }
                        break;
#                   else
                    case V4L2_BUF_TYPE_VIDEO_OUTPUT:
                    case V4L2_BUF_TYPE_VIDEO_CAPTURE:
                        if(print){
                            printf("ERROR, Buffers(%s), setting pixelformat('%c%c%c%c') width(%u) height(%u) sizeimage(%u) bytesperline(%u) returned(%d).\n", obj->name, pixelformatBytes[0], pixelformatBytes[1], pixelformatBytes[2], pixelformatBytes[3], obj->sp->width, obj->sp->height, obj->sp->sizeimage, obj->sp->bytesperline, rr);
                        }
                        break;
#                   endif
                    default:
                        break;
                }
            } else {
                switch(obj->type){
#                   ifdef K_USE_MPLANE
                    case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
                    case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
                        if(print){
                            printf("Buffers(%s), obtained pixelformat('%c%c%c%c') width(%u) height(%u).\n", obj->name, pixelformatBytes[0], pixelformatBytes[1], pixelformatBytes[2], pixelformatBytes[3], obj->mp->width, obj->mp->height);
                        }
                        obj->pixelformat = obj->mp->pixelformat;
                        obj->width  = obj->mp->width;
                        obj->height = obj->mp->height;
                        {
                            int i; for( i = 0; i < obj->mp->num_planes; i++){
                                struct v4l2_plane_pix_format* pp = &obj->mp->plane_fmt[i];
                                if(print){
                                    printf("    plane #%d, sizeimage(%u) bytesperline(%u).\n", (i + 1), pp->sizeimage, pp->bytesperline);
                                }
                            }
                        }
                        //init record for buffers requests
                        {
                            struct v4l2_buffer* srchBuff = &obj->srchBuff;
                            memset(srchBuff, 0, sizeof(*srchBuff));
                            if(obj->srchPlanes != NULL){
                                free(obj->srchPlanes);
                                obj->srchPlanes = NULL;
                            }
                            if(obj->mp->num_planes > 0){
                                obj->srchPlanes = malloc(sizeof(struct v4l2_plane) * obj->mp->num_planes);
                                if(obj->srchPlanes != NULL){
                                    memset(obj->srchPlanes, 0, sizeof(struct v4l2_plane) * obj->mp->num_planes);
                                }
                            }
                            srchBuff->length = obj->mp->num_planes;
                            srchBuff->m.planes = obj->srchPlanes;
                        }
                        break;
#                   else
                    case V4L2_BUF_TYPE_VIDEO_OUTPUT:
                    case V4L2_BUF_TYPE_VIDEO_CAPTURE:
                        if(print){
                            printf("Buffers(%s), obtained pixelformat('%c%c%c%c') width(%u) height(%u) sizeimage(%u) bytesperline(%u).\n", obj->name, pixelformatBytes[0], pixelformatBytes[1], pixelformatBytes[2], pixelformatBytes[3], obj->sp->width, obj->sp->height, obj->sp->sizeimage, obj->sp->bytesperline);
                        }
                        obj->pixelformat = obj->sp->pixelformat;
                        obj->width  = obj->sp->width;
                        obj->height = obj->sp->height;
                        //init record for buffers requests
                        {
                            struct v4l2_buffer* srchBuff = &obj->srchBuff;
                            memset(srchBuff, 0, sizeof(*srchBuff));
                        }
                        break;
#                   endif
                    default:
                        break;
                }
                //
                r = 0;
            }
        }
        //Enumerate controls
        //
        //TODO: query profiles and idc-levels supported
        //VIDIOC_QUERYCTRL
        //
        {
            v4lDevice_queryControls(fd, print);
        }
    }
    return r;
}

int Buffers_getCompositionRect(STBuffers* obj, int fd, STFbRect* dstRect){
    int r = -1;
    struct v4l2_selection sel;
    memset(&sel, 0, sizeof(sel));
    sel.type   = obj->type;
    sel.target = V4L2_SEL_TGT_COMPOSE;
    //try
    int rr; CALL_IOCTL(rr, v4l2_ioctl(fd, VIDIOC_G_SELECTION, &sel));
    if(rr != 0){
        printf("Buffers(%s), get-crop returned(%d).\n", obj->name, rr);
        //Note: linux doc, says that some messy drivers only accept one type or the other.
        //      retry with equivalent type.
    } else {
        printf("Buffers(%s), get-crop: x(%d, +%d) y(%d, +%d).\n", obj->name, sel.r.left, sel.r.width, sel.r.top, sel.r.height);
        if(dstRect != NULL){
            dstRect->x = sel.r.left;
            dstRect->y = sel.r.top;
            dstRect->width = sel.r.width;
            dstRect->height = sel.r.height;
        }
        r = 0;
    }
    return r;
}

int Buffers_allocBuffs(STBuffers* obj, int fd, int ammount, const int print){
    int r = -1;
    struct v4l2_requestbuffers buf;
    memset(&buf, 0, sizeof(buf));
    //
    buf.count = ammount;
    buf.type = obj->type;
    buf.memory = V4L2_MEMORY_MMAP;
    //
    int rr; CALL_IOCTL(rr, v4l2_ioctl(fd, VIDIOC_REQBUFS, &buf));
    if(rr != 0){
        printf("Buffers(%s), allocation of %d returned(%d).\n", obj->name, ammount, rr);
    } else {
        if(ammount != buf.count){
            printf("Buffers(%s), %d of %d allocated.\n", obj->name, buf.count, ammount);
        } else {
            printf("Buffers(%s), %d allocated.\n", obj->name, ammount);
        }
        //capabilities
#       if LINUX_VERSION_CODE >= KERNEL_VERSION(5,0,0)
        if(print){
#           ifdef V4L2_BUF_CAP_SUPPORTS_MMAP
            if((buf.capabilities & V4L2_BUF_CAP_SUPPORTS_MMAP)) printf("    capability: V4L2_BUF_CAP_SUPPORTS_MMAP.\n");
#           endif
#           ifdef V4L2_BUF_CAP_SUPPORTS_USERPTR
            if((buf.capabilities & V4L2_BUF_CAP_SUPPORTS_USERPTR)) printf("    capability: V4L2_BUF_CAP_SUPPORTS_USERPTR.\n");
#           endif
#           ifdef V4L2_BUF_CAP_SUPPORTS_DMABUF
            if((buf.capabilities & V4L2_BUF_CAP_SUPPORTS_DMABUF)) printf("    capability: V4L2_BUF_CAP_SUPPORTS_DMABUF.\n");
#           endif
#           ifdef V4L2_BUF_CAP_SUPPORTS_REQUESTS
            if((buf.capabilities & V4L2_BUF_CAP_SUPPORTS_REQUESTS)) printf("    capability: V4L2_BUF_CAP_SUPPORTS_REQUESTS.\n");
#           endif
#           ifdef V4L2_BUF_CAP_SUPPORTS_ORPHANED_BUFS
            if((buf.capabilities & V4L2_BUF_CAP_SUPPORTS_ORPHANED_BUFS)) printf("    capability: V4L2_BUF_CAP_SUPPORTS_ORPHANED_BUFS.\n");
#           endif
#           ifdef V4L2_BUF_CAP_SUPPORTS_M2M_HOLD_CAPTURE_BUF
            if((buf.capabilities & V4L2_BUF_CAP_SUPPORTS_M2M_HOLD_CAPTURE_BUF)) printf("    capability: V4L2_BUF_CAP_SUPPORTS_M2M_HOLD_CAPTURE_BUF.\n");
#           endif
#           ifdef V4L2_BUF_CAP_SUPPORTS_MMAP_CACHE_HINTS
            if((buf.capabilities & V4L2_BUF_CAP_SUPPORTS_MMAP_CACHE_HINTS)) printf("    capability: V4L2_BUF_CAP_SUPPORTS_MMAP_CACHE_HINTS.\n");
#           endif
            if(buf.capabilities != 0){
                printf("    capabilities: %d.\n", buf.capabilities);
            }
        }
#       endif
        //flags
#       if LINUX_VERSION_CODE >= KERNEL_VERSION(6,0,0)
        if(print){
#           ifdef V4L2_MEMORY_FLAG_NON_COHERENT
            if((buf.flags & V4L2_MEMORY_FLAG_NON_COHERENT)) printf("    flag: V4L2_MEMORY_FLAG_NON_COHERENT.\n");
#           endif
            if(buf.flags != 0){
                printf("    flags: %d.\n", buf.flags);
            }
        }
#       endif
        //map
        r = 0;
        {
            if(obj->arr != NULL){
                int i; for(i = 0; i < obj->use; i++){
                    Buffer_release(&obj->arr[i]);
                }
                free(obj->arr);
                obj->arr = NULL;
            }
            obj->use            = 0;
            obj->sz             = 0;
            obj->enqueuedCount  = 0;
            obj->lastDequeued   = NULL;
            obj->isLastDequeuedCloned = 0;
        }
        //
        {
            obj->sz = buf.count;
            if(obj->sz > 0){
                obj->arr = (STBuffer*)malloc(sizeof(STBuffer) * obj->sz);
                if(obj->arr == NULL){
                    printf("Buffers(%s), allocBuffs, malloc fail.\n", obj->name);
                    r = -1;
                } else {
                    int i; for(i = 0; i < obj->sz; i++){
                        STBuffer* b = &obj->arr[i];
                        Buffer_init(b);
                        b->index = i;
                        IF_DEBUG(b->dbg.indexPlusOne = (b->index + 1);)
                        obj->use++;
                    }
                }
            }
        }
        //reset times
        {
            obj->msWithoutEnqueuing = 0;    //ms without buffers enqueued
            obj->msWithoutDequeuing = 0;    //ms without buffer dequeued
        }
    }
    return r;
}

int Buffers_export(STBuffers* obj, int fd){
    int r = 0;
    int i; for(i = 0; i < obj->use; i++){
        STBuffer* buffer = &obj->arr[i];
        struct v4l2_buffer* srchBuff = &obj->srchBuff;
        K_ASSERT(buffer->dbg.indexPlusOne == (buffer->index + 1))
        //prepare search record
        memset(srchBuff, 0, sizeof(*srchBuff));
        srchBuff->index = i;
        srchBuff->type   = obj->type;
        srchBuff->memory = V4L2_MEMORY_MMAP;
        switch(obj->type){
#           ifdef K_USE_MPLANE
            case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
            case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
                if(obj->srchPlanes != NULL && obj->mp->num_planes > 0){
                    memset(obj->srchPlanes, 0, sizeof(struct v4l2_plane) * obj->mp->num_planes);
                }
                srchBuff->length = obj->mp->num_planes;
                srchBuff->m.planes = obj->srchPlanes;
                break;
#           else
            case V4L2_BUF_TYPE_VIDEO_OUTPUT:
            case V4L2_BUF_TYPE_VIDEO_CAPTURE:
                srchBuff->length = obj->sp->sizeimage;
                break;
#           endif
            default:
                break;
        }
        //
        int rr; CALL_IOCTL(rr, v4l2_ioctl(fd, VIDIOC_QUERYBUF, srchBuff));
        if(rr != 0){
            printf("Buffers(%s), VIDIOC_QUERYBUF returned(%d).\n", obj->name, rr);
            r = -1;
            break;
        } else {
            int planesSz = 0;
            switch(obj->type){
#               ifdef K_USE_MPLANE
                case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
                case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
                    planesSz = srchBuff->length;
                    break;
#               else
                case V4L2_BUF_TYPE_VIDEO_OUTPUT:
                case V4L2_BUF_TYPE_VIDEO_CAPTURE:
                    planesSz = 1;
                    break;
#               endif
                default:
                    break;
            }
            buffer->index = i;
            //reset planes array
            if(buffer->planesSz != planesSz){
                buffer->planesSz   = planesSz;
                if(buffer->planes != NULL){
                    free(buffer->planes);
                    buffer->planes = NULL;
                }
                if(buffer->planesSz > 0){
                    buffer->planes  = malloc(sizeof(STPlane) * buffer->planesSz);
                    int j; for(j = 0; j < buffer->planesSz; j++){
                        STPlane* plane  = &buffer->planes[j];
                        Plane_init(plane);
                    }
                }
            }
            //apply
            int j; for(j = 0; j < buffer->planesSz; j++){
                STPlane* plane  = &buffer->planes[j];
                plane->fd       = -1;
                //Export buffer (for DMA)
                {
                    struct v4l2_exportbuffer expbuf;
                    memset(&expbuf, 0, sizeof(expbuf));
                    //
                    expbuf.type = obj->type;
                    expbuf.index = i;
                    expbuf.plane = j;
                    expbuf.flags = O_RDWR; //O_CLOEXEC, O_RDONLY, O_WRONLY, and O_RDWR
                    //
                    const int ret = v4l2_ioctl(fd, VIDIOC_EXPBUF, &expbuf);
                    if (ret != 0){
                        if(errno == EINVAL){
                            printf("Buffers(%s) (#%d/%d) export is not supported.\n", obj->name, (i + 1), obj->use);
                        } else {
                            printf("Buffers(%s) (#%d/%d) plane(#%d/%d) export for DMA returned(%d).\n", obj->name, (i + 1), obj->use, j + 1, buffer->planesSz, ret);
                            r = -1;
                        }
                    } else {
                        printf("Buffers(%s) (#%d/%d) plane(#%d/%d) exported for DMA file(%d) dma(%d).\n", obj->name, (i + 1), obj->use, j + 1, buffer->planesSz, fd, expbuf.fd);
                        plane->fd = expbuf.fd;
                    }
                }
            }
        }
    }
    return r;
}

int Buffers_mmap(STBuffers* obj, int fd){
    int r = 0;
    int i; for(i = 0; i < obj->use; i++){
        STBuffer* buffer = &obj->arr[i];
        struct v4l2_buffer* srchBuff = &obj->srchBuff;
        K_ASSERT(buffer->dbg.indexPlusOne == (buffer->index + 1))
        //prepare search record
        memset(srchBuff, 0, sizeof(*srchBuff));
        srchBuff->index  = i;
        srchBuff->type   = obj->type;
        srchBuff->memory = V4L2_MEMORY_MMAP;
        switch(obj->type){
#           ifdef K_USE_MPLANE
            case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
            case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
                if(obj->srchPlanes != NULL && obj->mp->num_planes > 0){
                    memset(obj->srchPlanes, 0, sizeof(struct v4l2_plane) * obj->mp->num_planes);
                }
                srchBuff->length = obj->mp->num_planes;
                srchBuff->m.planes = obj->srchPlanes;
                break;
#           else
            case V4L2_BUF_TYPE_VIDEO_OUTPUT:
            case V4L2_BUF_TYPE_VIDEO_CAPTURE:
                srchBuff->length = obj->sp->sizeimage;
                break;
#           endif
            default:
                break;
        }
        //
        int rr; CALL_IOCTL(rr, v4l2_ioctl(fd, VIDIOC_QUERYBUF, srchBuff));
        if(rr != 0){
            printf("Buffers(%s), VIDIOC_QUERYBUF returned(%d).\n", obj->name, rr);
            r = -1;
        } else {
            int planesSz = 0, firstPlaneLen = 0, firstPlaneOffset = 0, firstPlaneFd = -1;
            switch(obj->type){
#               ifdef K_USE_MPLANE
                case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
                case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
                    planesSz = srchBuff->length;
                    if(planesSz > 0){
                        firstPlaneLen       = srchBuff->m.planes[0].length;
                        firstPlaneOffset    = srchBuff->m.planes[0].m.mem_offset;
                        firstPlaneFd        = srchBuff->m.planes[0].m.fd; //.m. are union
                    }
                    break;
#               else
                case V4L2_BUF_TYPE_VIDEO_OUTPUT:
                case V4L2_BUF_TYPE_VIDEO_CAPTURE:
                    planesSz = 1;
                    firstPlaneLen       = srchBuff->length;
                    firstPlaneOffset    = srchBuff->m.offset;   //.m. are union
                    firstPlaneFd        = srchBuff->m.fd;       //.m. are union
                    break;
#               endif
                default:
                    break;
            }
            //reset planes array
            buffer->index = i;
            if(buffer->planesSz != planesSz){
                buffer->planesSz   = planesSz;
                if(buffer->planes != NULL){
                    free(buffer->planes);
                    buffer->planes = NULL;
                }
                if(buffer->planesSz > 0){
                    buffer->planes  = malloc(sizeof(STPlane) * buffer->planesSz);
                    int j; for(j = 0; j < buffer->planesSz; j++){
                        STPlane* plane  = &buffer->planes[j];
                        Plane_init(plane);
                    }
                }
            }
            //apply
            int j; for(j = 0; j < buffer->planesSz; j++){
                STPlane* plane  = &buffer->planes[j];
                unsigned int pLength    = (j == 0 ? firstPlaneLen : srchBuff->m.planes[j].length);
                unsigned int pMemOffset = (j == 0 ? firstPlaneOffset : srchBuff->m.planes[j].m.mem_offset);
                int enumFd              = (j == 0 ? firstPlaneFd : srchBuff->m.planes[j].m.fd); //fields in '.m.' are union
                //mmap
                {
                    /* offset for mmap() must be page aligned */
                    off_t pa_offset = pMemOffset & ~(sysconf(_SC_PAGE_SIZE) - 1);
                    size_t pa_len = pLength + pMemOffset - pa_offset;
                    if(pa_len != pLength){
                        printf("Buffers(%s) (#%d/%d) plane(#%d/%d) is not page aligned lenght(%d) correctedLen(%d).\n", obj->name, (i + 1), obj->use, j + 1, buffer->planesSz, (unsigned int)pLength, (unsigned int)pa_len);
                        r = -1;
                    }
#                   ifdef K_IS_JETSON_NANO
                    int fddd = (plane->fd >= 0 ? plane->fd : fd); //whyyyyyyyy?
#                   else
                    int fddd = (/*plane->fd >= 0 ? plane->fd :*/ fd); //whyyyyyyyy?
#                   endif
                    void* rrmap = v4l2_mmap(NULL, pLength, PROT_READ | PROT_WRITE, MAP_SHARED, fddd, pMemOffset);
                    if(rrmap == MAP_FAILED){
                        if(errno == EACCES) { printf("ERROR, buffers(%s) (#%d/%d) plane(#%d/%d) map failed to myFd(%d) enumFd(%d) length(%d) mem_offset(%d): EACCES.\n", obj->name, (i + 1), obj->use, j + 1, buffer->planesSz, plane->fd, enumFd, pLength, pMemOffset); }
                        if(errno == EAGAIN) { printf("ERROR, buffers(%s) (#%d/%d) plane(#%d/%d) map failed to myFd(%d) enumFd(%d) length(%d) mem_offset(%d): EAGAIN.\n", obj->name, (i + 1), obj->use, j + 1, buffer->planesSz, plane->fd, enumFd, pLength, pMemOffset); }
                        if(errno == EBADF) { printf("ERROR, buffers(%s) (#%d/%d) plane(#%d/%d) map failed to myFd(%d) enumFd(%d) length(%d) mem_offset(%d): EBADF.\n", obj->name, (i + 1), obj->use, j + 1, buffer->planesSz, plane->fd, enumFd, pLength, pMemOffset); }
                        if(errno == EEXIST) { printf("ERROR, buffers(%s) (#%d/%d) plane(#%d/%d) map failed to myFd(%d) enumFd(%d) length(%d) mem_offset(%d): EEXIST.\n", obj->name, (i + 1), obj->use, j + 1, buffer->planesSz, plane->fd, enumFd, pLength, pMemOffset); }
                        if(errno == EINVAL) { printf("ERROR, buffers(%s) (#%d/%d) plane(#%d/%d) map failed to myFd(%d) enumFd(%d) length(%d) mem_offset(%d): EINVAL.\n", obj->name, (i + 1), obj->use, j + 1, buffer->planesSz, plane->fd, enumFd, pLength, pMemOffset); }
                        if(errno == ENFILE) { printf("ERROR, buffers(%s) (#%d/%d) plane(#%d/%d) map failed to myFd(%d) enumFd(%d) length(%d) mem_offset(%d): ENFILE.\n", obj->name, (i + 1), obj->use, j + 1, buffer->planesSz, plane->fd, enumFd, pLength, pMemOffset); }
                        if(errno == ENODEV) { printf("ERROR, buffers(%s) (#%d/%d) plane(#%d/%d) map failed to myFd(%d) enumFd(%d) length(%d) mem_offset(%d): ENODEV.\n", obj->name, (i + 1), obj->use, j + 1, buffer->planesSz, plane->fd, enumFd, pLength, pMemOffset); }
                        if(errno == ENOMEM) { printf("ERROR, buffers(%s) (#%d/%d) plane(#%d/%d) map failed to myFd(%d) enumFd(%d) length(%d) mem_offset(%d): ENOMEM.\n", obj->name, (i + 1), obj->use, j + 1, buffer->planesSz, plane->fd, enumFd, pLength, pMemOffset); }
                        if(errno == EOVERFLOW) { printf("ERROR, buffers(%s) (#%d/%d) plane(#%d/%d) map failed to myFd(%d) enumFd(%d) length(%d) mem_offset(%d): EOVERFLOW.\n", obj->name, (i + 1), obj->use, j + 1, buffer->planesSz, plane->fd, enumFd, pLength, pMemOffset); }
                        if(errno == EPERM) { printf("ERROR, buffers(%s) (#%d/%d) plane(#%d/%d) map failed to myFd(%d) enumFd(%d) length(%d) mem_offset(%d): EPERM.\n", obj->name, (i + 1), obj->use, j + 1, buffer->planesSz, plane->fd, enumFd, pLength, pMemOffset); }
                        if(errno == ETXTBSY) { printf("ERROR, buffers(%s) (#%d/%d) plane(#%d/%d) map failed to myFd(%d) enumFd(%d) length(%d) mem_offset(%d): ETXTBSY.\n", obj->name, (i + 1), obj->use, j + 1, buffer->planesSz, plane->fd, enumFd, pLength, pMemOffset); }
#                       ifdef SIGSEGV
                        if(errno == SIGSEGV) { printf("ERROR, buffers(%s) (#%d/%d) plane(#%d/%d) map failed to myFd(%d) enumFd(%d) length(%d) mem_offset(%d): SIGSEGV.\n", obj->name, (i + 1), obj->use, j + 1, buffer->planesSz, plane->fd, enumFd, pLength, pMemOffset); }
#                       endif
#                       ifdef SIGBUS
                        if(errno == SIGBUS) { printf("ERROR, buffers(%s) (#%d/%d) plane(#%d/%d) map failed to myFd(%d) enumFd(%d) length(%d) mem_offset(%d): SIGBUS.\n", obj->name, (i + 1), obj->use, j + 1, buffer->planesSz, plane->fd, enumFd, pLength, pMemOffset); }
#                       endif
                        r = -1;
                        break;
                    } else {
                        printf("Buffers(%s) (#%d/%d) plane(#%d/%d) mapped to (%llu) myFd(%d) enumFd(%d) length(%d) mem_offset(%d).\n", obj->name, (i + 1), obj->use, j + 1, buffer->planesSz, (unsigned long)rrmap, plane->fd, enumFd, pLength, pMemOffset);
                        plane->dataPtrIsExternal = 1;
                        plane->dataPtr  = (unsigned char*)rrmap;
                        plane->length   = pLength;
                        plane->memOffset = pMemOffset;
                    }
                }
            }
        }
    }
    return r;
}

int Buffers_unmap(STBuffers* obj, int fd){
    int r = 0;
    int i; for(i = 0; i < obj->use; i++){
        STBuffer* buffer = &obj->arr[i];
        K_ASSERT(buffer->dbg.indexPlusOne == (buffer->index + 1))
        //
        if(buffer->planes != NULL){
            int j; for(j = 0; j < buffer->planesSz; j++){
                STPlane* plane  = &buffer->planes[j];
                //unmap memory
                if(plane->dataPtr != NULL && plane->dataPtrIsExternal){
                    int rr = v4l2_munmap(plane->dataPtr, plane->length);
                    if(rr == 0){
                        printf("Buffers(%s) (#%d/%d) plane(#%d/%d) unmapped addr(%llu) len(%u).\n", obj->name, (i + 1), obj->use, (j + 1), buffer->planesSz, (unsigned long)plane->dataPtr, plane->length);
                    } else {
                        printf("ERROR, bufers(%s) munmap returned(%d) for buffer(#%d/%d) plane(#%d/%d) addr(%llu) len(%u).\n", obj->name, rr, (i + 1), obj->use, (j + 1), buffer->planesSz, (unsigned long)plane->dataPtr, plane->length);
                        r = -1;
                    }
                    plane->dataPtr = NULL;
                }
                plane->dataPtrIsExternal = 0;
                //close DMA
                if(plane->fd >= 0){
                    close(plane->fd);
                    plane->fd = -1;
                }
                //
                Plane_release(plane);
            }
            //free planes
            free(buffer->planes);
            buffer->planes = NULL;
        }
        buffer->planesSz = 0;
    }
    return r;
}

int Buffers_enqueueMinimun(STBuffers* obj, int fd, const int minimun){
    struct v4l2_buffer* srchBuff = &obj->srchBuff;
    while(obj->enqueuedCount < minimun){
        STBuffer* bufferQueued = NULL;
        int i; for(i = 0; i < obj->use && obj->enqueuedCount < minimun; i++){
            STBuffer* buffer = &obj->arr[i];
            K_ASSERT(buffer->dbg.indexPlusOne == (buffer->index + 1))
            if(!buffer->isQueued){
                //struct v4l2_format* fm = &obj->fm; //ToDo: remove
                //prepare search record
                memset(srchBuff, 0, sizeof(*srchBuff));
                srchBuff->index  = i;
                srchBuff->type   = obj->type;
                srchBuff->memory = V4L2_MEMORY_MMAP;
                K_ASSERT(!buffer->isQueued)
                switch(obj->type){
#                   ifdef K_USE_MPLANE
                    case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
                    case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
                        if(obj->srchPlanes != NULL && obj->mp->num_planes > 0){
                            memset(obj->srchPlanes, 0, sizeof(struct v4l2_plane) * obj->mp->num_planes);
                        }
                        srchBuff->length = obj->mp->num_planes;
                        srchBuff->m.planes = obj->srchPlanes;
                        //set data
                        {
                            int i; for(i = 0; i < srchBuff->length; i++){
                                srchBuff->m.planes[i].bytesused = buffer->planes[i].used;
                            }
                        }
                        break;
#                   else
                    case V4L2_BUF_TYPE_VIDEO_OUTPUT:
                    case V4L2_BUF_TYPE_VIDEO_CAPTURE:
                        srchBuff->length = obj->sp->sizeimage;
                        //set data
                        srchBuff->bytesused = (buffer->planesSz > 0 ? buffer->planes[0].used : 0);
                        break;
#                   endif
                    default:
                        break;
                }
                //action
                {
                    int rr2; CALL_IOCTL(rr2, v4l2_ioctl(fd, VIDIOC_QBUF, &obj->srchBuff));
                    if(rr2 != 0){
                        printf("Buffers(%s), #%d/%d queeing returned(%d).\n", obj->name, (i + 1), obj->use, rr2);
                    } else {
                        printf("Buffers(%s), #%d/%d queued.\n", obj->name, (i + 1), obj->use);
                        buffer->isQueued = 1;
                        obj->enqueuedCount++; K_ASSERT(obj->enqueuedCount >= 0 && obj->enqueuedCount <= obj->use);
                        bufferQueued = buffer;
                        //reset times
                        obj->msWithoutEnqueuing = 0;
                    }
                }
            }
        }
        //stop if no buffer is available
        if(bufferQueued == NULL){
            break;
        }
    }
    return (obj->enqueuedCount >= minimun ? 0 : -1);
}

//get a buffer not enqueued yet
int Buffers_getUnqueued(STBuffers* obj, STBuffer** dstBuff, STBuffer* ignoreThis){
    int r = -1;
    int i; for(i = 0; i < obj->use; i++){
        STBuffer* buffer = &obj->arr[i];
        //
        K_ASSERT(buffer->dbg.indexPlusOne == (buffer->index + 1))
        //
        if(!buffer->isQueued && buffer != ignoreThis){
            if(dstBuff != NULL){
                *dstBuff = buffer;
            }
            r = 0;
            break;
        }
    }
    return r;
}

//add to queue
int Buffers_enqueue(STBuffers* obj, int fd, STBuffer* buffer, const struct timeval* srcTimestamp){
    int r = -1;
    K_ASSERT(buffer->dbg.indexPlusOne == (buffer->index + 1))
    K_ASSERT(!buffer->isQueued)
    if(!buffer->isQueued){
        struct v4l2_buffer* srchBuff = &obj->srchBuff;
        //prepare search record
        memset(srchBuff, 0, sizeof(*srchBuff));
        srchBuff->index  = buffer->index;
        srchBuff->type   = obj->type;
        srchBuff->memory = V4L2_MEMORY_MMAP;
        if(srcTimestamp != NULL){
            srchBuff->flags |= V4L2_BUF_FLAG_TIMESTAMP_COPY;
            srchBuff->timestamp = *srcTimestamp;
        }
        K_ASSERT(!buffer->isQueued)
        switch(obj->type){
#           ifdef K_USE_MPLANE
            case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
            case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
                if(obj->srchPlanes != NULL && obj->mp->num_planes > 0){
                    memset(obj->srchPlanes, 0, sizeof(struct v4l2_plane) * obj->mp->num_planes);
                }
                srchBuff->length = obj->mp->num_planes;
                srchBuff->m.planes = obj->srchPlanes;
                //set data
                {
                    int i; for(i = 0; i < srchBuff->length; i++){
                        srchBuff->m.planes[i].bytesused = buffer->planes[i].used;
                    }
                }
                break;
#           else
            case V4L2_BUF_TYPE_VIDEO_OUTPUT:
            case V4L2_BUF_TYPE_VIDEO_CAPTURE:
                srchBuff->length = obj->sp->sizeimage;
                //set data
                srchBuff->bytesused = (buffer->planesSz > 0 ? buffer->planes[0].used : 0);
                break;
#           endif
            default:
                break;
        }
        //action
        {
            int rr2; CALL_IOCTL(rr2, v4l2_ioctl(fd, VIDIOC_QBUF, &obj->srchBuff));
            if(rr2 != 0){
                printf("Buffers(%s), queueing buffer(#%d/%d) returned (%d).\n", obj->name, srchBuff->index + 1, obj->use, rr2);
            } else {
                //printf("Buffers(%s), queueing new-buffer(#%d/%d) success.\n", obj->name, srchBuff->index + 1, obj->srcBuffsSz);
                buffer->isQueued = 1;
                obj->enqueuedCount++; K_ASSERT(obj->enqueuedCount >= 0 && obj->enqueuedCount <= obj->use)
                //reset times
                obj->msWithoutEnqueuing = 0;
                //
                r = 0;
            }
        }
    }
    return r;
}

//remove from queue
int Buffers_dequeue(STBuffers* obj, int fd, STBuffer** dstBuff, struct timeval* dstTimestamp){
    int r = -1;
    //prepare search record
    struct v4l2_buffer* srchBuff = &obj->srchBuff;
    memset(srchBuff, 0, sizeof(*srchBuff));
    srchBuff->type   = obj->type;
    srchBuff->memory = V4L2_MEMORY_MMAP;
    switch(obj->type){
#       ifdef K_USE_MPLANE
        case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
        case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
            if(obj->srchPlanes != NULL && obj->mp->num_planes > 0){
                memset(obj->srchPlanes, 0, sizeof(struct v4l2_plane) * obj->mp->num_planes);
            }
            srchBuff->length = obj->mp->num_planes;
            srchBuff->m.planes = obj->srchPlanes;
            break;
#       else
        case V4L2_BUF_TYPE_VIDEO_OUTPUT:
        case V4L2_BUF_TYPE_VIDEO_CAPTURE:
            srchBuff->length = obj->sp->sizeimage;
            break;
#       endif
        default:
            break;
    }
    //returns 'EPIPE' if the flag-last was already emited
    int rr; CALL_IOCTL(rr, v4l2_ioctl(fd, VIDIOC_DQBUF, &obj->srchBuff));
    if(rr != 0){
        switch (errno) {
            case EAGAIN:
                //Non-blocking IO
                printf("Buffers(%s), Unqueueing buffer (returned EAGAIN, no buffer is ready).\n", obj->name);
                break;
            case EINVAL:
                //The buffer type is not supported, or the index is out of bounds, or no buffers have been allocated yet, or the userptr or length are invalid.
                printf("Buffers(%s), Unqueueing buffer (returned EINVAL, no buffer is ready).\n", obj->name);
                break;
            case EIO:
                //VIDIOC_DQBUF failed due to an internal error. Can also indicate temporary problems like signal loss.
                printf("Buffers(%s), Unqueueing buffer (returned EIO, no buffer is ready).\n", obj->name);
                break;
            case EPIPE:
                //VIDIOC_DQBUF returns this on an empty capture queue for mem2mem codecs if a buffer with the V4L2_BUF_FLAG_LAST was already dequeued and no new buffers are expected to become available.
                printf("Buffers(%s), Unqueueing buffer (returned EPIPE, last buffer given, dst-restart is required).\n", obj->name);
                /*if(wasEPIPE != NULL){
                    *wasEPIPE = 1;
                }*/
                break;
            default:
                printf("Buffers(%s), Unqueueing buffer (returned %d, no buffer is ready).\n", obj->name, rr);
                break;
        }
    } else if(srchBuff->index >= obj->use){
        printf("ERROR, Buffers(%s), dequeued returned an invalid buffer-index.\n", obj->name);
    } else {
        //printf("Unqueueing dst-buffer(#%d/%d) returned filled.\n", obj->name, (srchBuff->index + 1), obj->use);
        STBuffer* buffer = &obj->arr[srchBuff->index];
        //
        K_ASSERT(buffer->dbg.indexPlusOne == (buffer->index + 1))
        K_ASSERT(buffer->isQueued)
        //sync state
        obj->enqueuedCount--; K_ASSERT(obj->enqueuedCount >= 0 && obj->enqueuedCount <= obj->use)
        obj->lastDequeued = buffer;
        obj->isLastDequeuedCloned = 0;
        //reset times
        obj->msWithoutDequeuing = 0;
        //sync record
        buffer->isQueued = 0;
        switch(obj->type){
#           ifdef K_USE_MPLANE
            case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
            case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
                {
                    int i; for(i = 0; i < srchBuff->length; i++){
                        buffer->planes[i].used = srchBuff->m.planes[i].bytesused;
                        buffer->planes[i].bytesPerLn = obj->mp->plane_fmt[i].bytesperline;
                    }
                }
                break;
#           else
            case V4L2_BUF_TYPE_VIDEO_OUTPUT:
            case V4L2_BUF_TYPE_VIDEO_CAPTURE:
                if(buffer->planesSz > 0){
                    buffer->planes[0].used = srchBuff->bytesused;
                }
                break;
#           endif
            default:
                break;
        }
        //
        /*if((srchBuff->flags & V4L2_BUF_FLAG_LAST)){
         if(obj->evntsPendsUse == 0){
         printf("Buffers(%s), V4L2_BUF_FLAG_LAST found but unexpected (no active drainig-seq).\n", obj->name);
         } else {
         printf("Buffers(%s), drain completed (V4L2_BUF_FLAG_LAST).\n", obj->name);
         }
         if(dstWasDrainLastBuff != NULL){
         *dstWasDrainLastBuff = 1;
         }
         }*/
        //flags
        /*{
         if((srchBuff->flags & V4L2_BUF_FLAG_MAPPED)){ printf("    flag: V4L2_BUF_FLAG_MAPPED.\n"); }
         if((srchBuff->flags & V4L2_BUF_FLAG_QUEUED)){ printf("    flag: V4L2_BUF_FLAG_QUEUED.\n"); }
         if((srchBuff->flags & V4L2_BUF_FLAG_DONE)){ printf("    flag: V4L2_BUF_FLAG_DONE.\n"); }
         if((srchBuff->flags & V4L2_BUF_FLAG_ERROR)){ printf("    flag: V4L2_BUF_FLAG_ERROR.\n"); }
         #           ifdef V4L2_BUF_FLAG_IN_REQUEST
         if((srchBuff->flags & V4L2_BUF_FLAG_IN_REQUEST)){ printf("    flag: V4L2_BUF_FLAG_IN_REQUEST.\n"); }
         #           endif
         if((srchBuff->flags & V4L2_BUF_FLAG_KEYFRAME)){ printf("    flag: V4L2_BUF_FLAG_KEYFRAME.\n"); }
         if((srchBuff->flags & V4L2_BUF_FLAG_PFRAME)){ printf("    flag: V4L2_BUF_FLAG_PFRAME.\n"); }
         if((srchBuff->flags & V4L2_BUF_FLAG_BFRAME)){ printf("    flag: V4L2_BUF_FLAG_BFRAME.\n"); }
         if((srchBuff->flags & V4L2_BUF_FLAG_TIMECODE)){ printf("    flag: V4L2_BUF_FLAG_TIMECODE.\n"); }
         if((srchBuff->flags & V4L2_BUF_FLAG_PREPARED)){ printf("    flag: V4L2_BUF_FLAG_PREPARED.\n"); }
         if((srchBuff->flags & V4L2_BUF_FLAG_NO_CACHE_INVALIDATE)){ printf("    flag: V4L2_BUF_FLAG_NO_CACHE_INVALIDATE.\n"); }
         if((srchBuff->flags & V4L2_BUF_FLAG_NO_CACHE_CLEAN)){ printf("    flag: V4L2_BUF_FLAG_NO_CACHE_CLEAN.\n"); }
         #           ifdef V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF
         if((srchBuff->flags & V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF)){ printf("    flag: V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF.\n"); }
         #           endif
         if((srchBuff->flags & V4L2_BUF_FLAG_LAST)){ printf("    flag: V4L2_BUF_FLAG_LAST.\n"); }
         #           ifdef V4L2_BUF_FLAG_REQUEST_FD
         if((srchBuff->flags & V4L2_BUF_FLAG_REQUEST_FD)){ printf("    flag: V4L2_BUF_FLAG_REQUEST_FD.\n"); }
         #           endif
         if((srchBuff->flags & V4L2_BUF_FLAG_TIMESTAMP_MASK)){ printf("    flag: V4L2_BUF_FLAG_TIMESTAMP_MASK.\n"); }
         if((srchBuff->flags & V4L2_BUF_FLAG_TIMESTAMP_UNKNOWN)){ printf("    flag: V4L2_BUF_FLAG_TIMESTAMP_UNKNOWN.\n"); }
         if((srchBuff->flags & V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC)){ printf("    flag: V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC.\n"); }
         if((srchBuff->flags & V4L2_BUF_FLAG_TIMESTAMP_COPY)){ printf("    flag: V4L2_BUF_FLAG_TIMESTAMP_COPY.\n"); }
         if((srchBuff->flags & V4L2_BUF_FLAG_TSTAMP_SRC_MASK)){ printf("    flag: V4L2_BUF_FLAG_TSTAMP_SRC_MASK.\n"); }
         if((srchBuff->flags & V4L2_BUF_FLAG_TSTAMP_SRC_EOF)){ printf("    flag: V4L2_BUF_FLAG_TSTAMP_SRC_EOF.\n"); }
         if((srchBuff->flags & V4L2_BUF_FLAG_TSTAMP_SRC_SOE)){ printf("    flag: V4L2_BUF_FLAG_TSTAMP_SRC_SOE.\n"); }
         }*/
        if(dstTimestamp != NULL && (srchBuff->flags & V4L2_BUF_FLAG_TIMESTAMP_COPY)){
            *dstTimestamp = srchBuff->timestamp;
        }
        //
        if(dstBuff != NULL){
            *dstBuff = buffer;
        }
        r = 0;
    }
    return r;
}

int Buffers_start(STBuffers* obj, int fd){
    int type = obj->type;
    int r; CALL_IOCTL(r, v4l2_ioctl(fd, VIDIOC_STREAMON, &type));
    if(r != 0){
        printf("Buffers(%s) start failed(%d).\n", obj->name, r);
    } else {
        obj->isExplicitON = 1;
        obj->isImplicitON = 1;
        //printf("Buffers_start success.\n");
    }
    return r;
}

int Buffers_stop(STBuffers* obj, int fd){
    int type = obj->type;
    int r; CALL_IOCTL(r, v4l2_ioctl(fd, VIDIOC_STREAMOFF, &type));
    if(r != 0){
        printf("Buffers(%s) stop failed(%d).\n", obj->name, r);
    } else {
        obj->isExplicitON = 0;
        obj->isImplicitON = 0;
        printf("Buffers(%s) stop success.\n", obj->name);
        //flag all buffers as dequeued
        {
            int i; for(i = 0; i < obj->use; i++){
                STBuffer* buffer = &obj->arr[i];
                buffer->isQueued = 0;
            }
            obj->enqueuedCount = 0;
        }
    }
    return r;
}

int Buffers_keepLastAsClone(STBuffers* obj, STBuffer* src){
    int r = -1;
    if(0 == Buffer_clone(&obj->lastDequeuedClone, src)){
        obj->isLastDequeuedCloned = 1;
        r = 0;
    }
    return r;
}

//

int v4lDevice_queryCaps(int fd, const int print){
    int r = -1;
    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    int rr; CALL_IOCTL(rr, v4l2_ioctl(fd, VIDIOC_QUERYCAP, &cap));
    if(rr != 0){
        printf("Src-stream VIDIOC_QUERYCAP failed(%d).\n", rr);
    } else {
        if(print){
            printf("----------------.\n");
            printf("---- DEVICE ----.\n");
            printf("----------------.\n");
            printf("Driver: '%s'.\n", cap.driver);
            printf("  Card: '%s'.\n", cap.card);
            printf("   Bus: '%s'.\n", cap.bus_info);
            printf("   Ver: %u.%u.%u.\n", (cap.version >> 16) & 0xFF, (cap.version >> 8) & 0xFF, cap.version & 0xFF);
            printf("   Cap: .\n");
#       ifdef V4L2_CAP_VIDEO_CAPTURE
            if((cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)){ printf("   Cap: V4L2_CAP_VIDEO_CAPTURE.\n"); }
#       endif
#       ifdef V4L2_CAP_VIDEO_CAPTURE_MPLANE
            if((cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE)){ printf("   Cap: V4L2_CAP_VIDEO_CAPTURE_MPLANE.\n"); }
#       endif
#       ifdef V4L2_CAP_VIDEO_OUTPUT
            if((cap.capabilities & V4L2_CAP_VIDEO_OUTPUT)){ printf("   Cap: V4L2_CAP_VIDEO_OUTPUT.\n"); }
#       endif
#       ifdef V4L2_CAP_VIDEO_OUTPUT_MPLANE
            if((cap.capabilities & V4L2_CAP_VIDEO_OUTPUT_MPLANE)){ printf("   Cap: V4L2_CAP_VIDEO_OUTPUT_MPLANE.\n"); }
#       endif
#       ifdef V4L2_CAP_VIDEO_M2M
            if((cap.capabilities & V4L2_CAP_VIDEO_M2M)){ printf("   Cap: V4L2_CAP_VIDEO_M2M.\n"); }
#       endif
#       ifdef V4L2_CAP_VIDEO_M2M_MPLANE
            if((cap.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE)){ printf("   Cap: V4L2_CAP_VIDEO_M2M_MPLANE.\n"); }
#       endif
#       ifdef V4L2_CAP_VIDEO_OVERLAY
            if((cap.capabilities & V4L2_CAP_VIDEO_OVERLAY)){ printf("   Cap: V4L2_CAP_VIDEO_OVERLAY.\n"); }
#       endif
#       ifdef V4L2_CAP_VBI_CAPTURE
            if((cap.capabilities & V4L2_CAP_VBI_CAPTURE)){ printf("   Cap: V4L2_CAP_VBI_CAPTURE.\n"); }
#       endif
#       ifdef V4L2_CAP_VBI_OUTPUT
            if((cap.capabilities & V4L2_CAP_VBI_OUTPUT)){ printf("   Cap: V4L2_CAP_VBI_OUTPUT.\n"); }
#       endif
#       ifdef V4L2_CAP_SLICED_VBI_CAPTURE
            if((cap.capabilities & V4L2_CAP_SLICED_VBI_CAPTURE)){ printf("   Cap: V4L2_CAP_SLICED_VBI_CAPTURE.\n"); }
#       endif
#       ifdef V4L2_CAP_SLICED_VBI_OUTPUT
            if((cap.capabilities & V4L2_CAP_SLICED_VBI_OUTPUT)){ printf("   Cap: V4L2_CAP_SLICED_VBI_OUTPUT.\n"); }
#       endif
#       ifdef V4L2_CAP_RDS_CAPTURE
            if((cap.capabilities & V4L2_CAP_RDS_CAPTURE)){ printf("   Cap: V4L2_CAP_RDS_CAPTURE.\n"); }
#       endif
#       ifdef V4L2_CAP_VIDEO_OUTPUT_OVERLAY
            if((cap.capabilities & V4L2_CAP_VIDEO_OUTPUT_OVERLAY)){ printf("   Cap: V4L2_CAP_VIDEO_OUTPUT_OVERLAY.\n"); }
#       endif
#       ifdef V4L2_CAP_HW_FREQ_SEEK
            if((cap.capabilities & V4L2_CAP_HW_FREQ_SEEK)){ printf("   Cap: V4L2_CAP_HW_FREQ_SEEK.\n"); }
#       endif
#       ifdef V4L2_CAP_RDS_OUTPUT
            if((cap.capabilities & V4L2_CAP_RDS_OUTPUT)){ printf("   Cap: V4L2_CAP_RDS_OUTPUT.\n"); }
#       endif
#       ifdef V4L2_CAP_TUNER
            if((cap.capabilities & V4L2_CAP_TUNER)){ printf("   Cap: V4L2_CAP_TUNER.\n"); }
#       endif
#       ifdef V4L2_CAP_AUDIO
            if((cap.capabilities & V4L2_CAP_AUDIO)){ printf("   Cap: V4L2_CAP_AUDIO.\n"); }
#       endif
#       ifdef V4L2_CAP_RADIO
            if((cap.capabilities & V4L2_CAP_RADIO)){ printf("   Cap: V4L2_CAP_RADIO.\n"); }
#       endif
#       ifdef V4L2_CAP_MODULATOR
            if((cap.capabilities & V4L2_CAP_MODULATOR)){ printf("   Cap: V4L2_CAP_MODULATOR.\n"); }
#       endif
#       ifdef V4L2_CAP_SDR_CAPTURE
            if((cap.capabilities & V4L2_CAP_SDR_CAPTURE)){ printf("   Cap: V4L2_CAP_SDR_CAPTURE.\n"); }
#       endif
#       ifdef V4L2_CAP_EXT_PIX_FORMAT
            if((cap.capabilities & V4L2_CAP_EXT_PIX_FORMAT)){ printf("   Cap: V4L2_CAP_EXT_PIX_FORMAT.\n"); }
#       endif
#       ifdef V4L2_CAP_SDR_OUTPUT
            if((cap.capabilities & V4L2_CAP_SDR_OUTPUT)){ printf("   Cap: V4L2_CAP_SDR_OUTPUT.\n"); }
#       endif
#       ifdef V4L2_CAP_READWRITE
            if((cap.capabilities & V4L2_CAP_READWRITE)){ printf("   Cap: V4L2_CAP_READWRITE.\n"); }
#       endif
#       ifdef V4L2_CAP_ASYNCIO
            if((cap.capabilities & V4L2_CAP_ASYNCIO)){ printf("   Cap: V4L2_CAP_ASYNCIO.\n"); }
#       endif
#       ifdef V4L2_CAP_STREAMING
            if((cap.capabilities & V4L2_CAP_STREAMING)){ printf("   Cap: V4L2_CAP_STREAMING.\n"); }
#       endif
#       ifdef V4L2_CAP_TOUCH
            if((cap.capabilities & V4L2_CAP_TOUCH)){ printf("   Cap: V4L2_CAP_TOUCH.\n"); }
#       endif
#       ifdef V4L2_CAP_DEVICE_CAPS
            if((cap.capabilities & V4L2_CAP_DEVICE_CAPS)){
                printf("   Cap: V4L2_CAP_DEVICE_CAPS.\n");
                {
#              ifdef V4L2_CAP_VIDEO_CAPTURE
                    if((cap.device_caps & V4L2_CAP_VIDEO_CAPTURE)){ printf("DevCap: V4L2_CAP_VIDEO_CAPTURE.\n"); }
#              endif
#              ifdef V4L2_CAP_VIDEO_CAPTURE_MPLANE
                    if((cap.device_caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE)){ printf("DevCap: V4L2_CAP_VIDEO_CAPTURE_MPLANE.\n"); }
#              endif
#              ifdef V4L2_CAP_VIDEO_OUTPUT
                    if((cap.device_caps & V4L2_CAP_VIDEO_OUTPUT)){ printf("DevCap: V4L2_CAP_VIDEO_OUTPUT.\n"); }
#              endif
#              ifdef V4L2_CAP_VIDEO_OUTPUT_MPLANE
                    if((cap.device_caps & V4L2_CAP_VIDEO_OUTPUT_MPLANE)){ printf("DevCap: V4L2_CAP_VIDEO_OUTPUT_MPLANE.\n"); }
#              endif
#              ifdef V4L2_CAP_VIDEO_M2M
                    if((cap.device_caps & V4L2_CAP_VIDEO_M2M)){ printf("DevCap: V4L2_CAP_VIDEO_M2M.\n"); }
#              endif
#              ifdef V4L2_CAP_VIDEO_M2M_MPLANE
                    if((cap.device_caps & V4L2_CAP_VIDEO_M2M_MPLANE)){ printf("DevCap: V4L2_CAP_VIDEO_M2M_MPLANE.\n"); }
#              endif
#              ifdef V4L2_CAP_VIDEO_OVERLAY
                    if((cap.device_caps & V4L2_CAP_VIDEO_OVERLAY)){ printf("DevCap: V4L2_CAP_VIDEO_OVERLAY.\n"); }
#              endif
#              ifdef V4L2_CAP_VBI_CAPTURE
                    if((cap.device_caps & V4L2_CAP_VBI_CAPTURE)){ printf("DevCap: V4L2_CAP_VBI_CAPTURE.\n"); }
#              endif
#              ifdef V4L2_CAP_VBI_OUTPUT
                    if((cap.device_caps & V4L2_CAP_VBI_OUTPUT)){ printf("DevCap: V4L2_CAP_VBI_OUTPUT.\n"); }
#              endif
#              ifdef V4L2_CAP_SLICED_VBI_CAPTURE
                    if((cap.device_caps & V4L2_CAP_SLICED_VBI_CAPTURE)){ printf("DevCap: V4L2_CAP_SLICED_VBI_CAPTURE.\n"); }
#              endif
#              ifdef V4L2_CAP_SLICED_VBI_OUTPUT
                    if((cap.device_caps & V4L2_CAP_SLICED_VBI_OUTPUT)){ printf("DevCap: V4L2_CAP_SLICED_VBI_OUTPUT.\n"); }
#              endif
#              ifdef V4L2_CAP_RDS_CAPTURE
                    if((cap.device_caps & V4L2_CAP_RDS_CAPTURE)){ printf("DevCap: V4L2_CAP_RDS_CAPTURE.\n"); }
#              endif
#              ifdef V4L2_CAP_VIDEO_OUTPUT_OVERLAY
                    if((cap.device_caps & V4L2_CAP_VIDEO_OUTPUT_OVERLAY)){ printf("DevCap: V4L2_CAP_VIDEO_OUTPUT_OVERLAY.\n"); }
#              endif
#              ifdef V4L2_CAP_HW_FREQ_SEEK
                    if((cap.device_caps & V4L2_CAP_HW_FREQ_SEEK)){ printf("DevCap: V4L2_CAP_HW_FREQ_SEEK.\n"); }
#              endif
#              ifdef V4L2_CAP_RDS_OUTPUT
                    if((cap.device_caps & V4L2_CAP_RDS_OUTPUT)){ printf("DevCap: V4L2_CAP_RDS_OUTPUT.\n"); }
#              endif
#              ifdef V4L2_CAP_TUNER
                    if((cap.device_caps & V4L2_CAP_TUNER)){ printf("DevCap: V4L2_CAP_TUNER.\n"); }
#              endif
#              ifdef V4L2_CAP_AUDIO
                    if((cap.device_caps & V4L2_CAP_AUDIO)){ printf("DevCap: V4L2_CAP_AUDIO.\n"); }
#              endif
#              ifdef V4L2_CAP_RADIO
                    if((cap.device_caps & V4L2_CAP_RADIO)){ printf("DevCap: V4L2_CAP_RADIO.\n"); }
#              endif
#              ifdef V4L2_CAP_MODULATOR
                    if((cap.device_caps & V4L2_CAP_MODULATOR)){ printf("DevCap: V4L2_CAP_MODULATOR.\n"); }
#              endif
#              ifdef V4L2_CAP_SDR_CAPTURE
                    if((cap.device_caps & V4L2_CAP_SDR_CAPTURE)){ printf("DevCap: V4L2_CAP_SDR_CAPTURE.\n"); }
#              endif
#              ifdef V4L2_CAP_EXT_PIX_FORMAT
                    if((cap.device_caps & V4L2_CAP_EXT_PIX_FORMAT)){ printf("DevCap: V4L2_CAP_EXT_PIX_FORMAT.\n"); }
#              endif
#              ifdef V4L2_CAP_SDR_OUTPUT
                    if((cap.device_caps & V4L2_CAP_SDR_OUTPUT)){ printf("DevCap: V4L2_CAP_SDR_OUTPUT.\n"); }
#              endif
#              ifdef V4L2_CAP_READWRITE
                    if((cap.device_caps & V4L2_CAP_READWRITE)){ printf("DevCap: V4L2_CAP_READWRITE.\n"); }
#              endif
#              ifdef V4L2_CAP_ASYNCIO
                    if((cap.device_caps & V4L2_CAP_ASYNCIO)){ printf("DevCap: V4L2_CAP_ASYNCIO.\n"); }
#              endif
#              ifdef V4L2_CAP_STREAMING
                    if((cap.device_caps & V4L2_CAP_STREAMING)){ printf("DevCap: V4L2_CAP_STREAMING.\n"); }
#              endif
#              ifdef V4L2_CAP_TOUCH
                    if((cap.device_caps & V4L2_CAP_TOUCH)){ printf("DevCap: V4L2_CAP_TOUCH.\n"); }
#              endif
#              ifdef V4L2_CAP_DEVICE_CAPS
                    if((cap.device_caps & V4L2_CAP_DEVICE_CAPS)){ printf("DevCap: V4L2_CAP_DEVICE_CAPS.\n"); }
#              endif
                }
            }
#       endif
        }
        r = 0;
    }
    return r;
}

int v4lDevice_queryControls(int fd, const int print){
    int controlsTotal = 0;
    int rr2 = 0;
    //
    //VIDIOC_QUERYCTRL vs VIDIOC_G_CTRL ?
    {
        //standard controls
        unsigned int cid = V4L2_CID_BASE;
        int reqsCountBase = 0, reqsCountPriv = 0;
        struct v4l2_queryctrl ctrl;
        memset(&ctrl, 0, sizeof(ctrl));
        for(cid = V4L2_CID_BASE; cid < V4L2_CID_LASTP1; cid++){
            ctrl.id = cid;
            rr2 = v4l2_ioctl(fd, VIDIOC_QUERYCTRL, &ctrl);
            if(rr2 == 0){
                v4lDevice_controlAnalyze(fd, &ctrl, print);
                controlsTotal++;
            }
            reqsCountBase++;
        }
        //private controls
        {
            unsigned int cid = V4L2_CID_PRIVATE_BASE;
            rr2 = 0;
            while(rr2 == 0){
                ctrl.id = cid;
                rr2 = v4l2_ioctl(fd, VIDIOC_QUERYCTRL, &ctrl);
                if(rr2 == 0){
                    v4lDevice_controlAnalyze(fd, &ctrl, print);
                    controlsTotal++;
                }
                //next
                cid++;
                reqsCountPriv++;
            }
        }
        if(print){
            printf("%d controls queried (%d standard, %d private requested).\n", controlsTotal, reqsCountBase, reqsCountPriv);
        }
    }
    {
        //standard controls
        unsigned int cid = V4L2_CID_BASE;
        struct v4l2_control ctrl;
        memset(&ctrl, 0, sizeof(ctrl));
        for(cid = V4L2_CID_BASE; cid < V4L2_CID_LASTP1; cid++){
            ctrl.id = cid;
            rr2 = v4l2_ioctl(fd, VIDIOC_G_CTRL, &ctrl);
            if(rr2 == 0){
                //printf("Control %d value: %d.\n", cid, ctrl.value);
                controlsTotal++;
            }
        }
        //private controls
        /*{
            rr2 = 0; ctrl.id = V4L2_CID_PRIVATE_BASE;
            while(rr2 == 0){
                rr2 = v4l2_ioctl(fd, VIDIOC_G_CTRL, &ctrl);
                if(rr2 == 0){
                    printf("Control %d value: %d.\n", ctrl.id, ctrl.value);
                    controlsTotal++;
                }
                //next
                ctrl.id++;
            }
        }*/
        if(print){
            printf("%d controls goten.\n", controlsTotal);
        }
    }
    return 0;
}

int v4lDevice_controlAnalyze(int fd, struct v4l2_queryctrl* ctrl, const int print){
    if(print){
        printf("Control: '%s' (%d, +%d, %d).\n", ctrl->name, ctrl->minimum, ctrl->step, ctrl->maximum);
        //type
        switch(ctrl->type){
            case V4L2_CTRL_TYPE_INTEGER: printf("    Type: V4L2_CTRL_TYPE_INTEGER.\n"); break;
            case V4L2_CTRL_TYPE_BOOLEAN: printf("    Type: V4L2_CTRL_TYPE_BOOLEAN.\n"); break;
            case V4L2_CTRL_TYPE_MENU: printf("    Type: V4L2_CTRL_TYPE_MENU.\n"); break;
            case V4L2_CTRL_TYPE_INTEGER_MENU: printf("    Type: V4L2_CTRL_TYPE_INTEGER_MENU.\n"); break;
            case V4L2_CTRL_TYPE_BITMASK: printf("    Type: V4L2_CTRL_TYPE_BITMASK.\n"); break;
            case V4L2_CTRL_TYPE_BUTTON: printf("    Type: V4L2_CTRL_TYPE_BUTTON.\n"); break;
            case V4L2_CTRL_TYPE_INTEGER64: printf("    Type: V4L2_CTRL_TYPE_INTEGER64.\n"); break;
            case V4L2_CTRL_TYPE_STRING: printf("    Type: V4L2_CTRL_TYPE_STRING.\n"); break;
            case V4L2_CTRL_TYPE_CTRL_CLASS: printf("    Type: V4L2_CTRL_TYPE_CTRL_CLASS.\n"); break;
            case V4L2_CTRL_TYPE_U8: printf("    Type: V4L2_CTRL_TYPE_U8.\n"); break;
            case V4L2_CTRL_TYPE_U16: printf("    Type: V4L2_CTRL_TYPE_U16.\n"); break;
            case V4L2_CTRL_TYPE_U32: printf("    Type: V4L2_CTRL_TYPE_U32.\n"); break;
            default: printf("    Type: unknown.\n"); break;
        }
        //flags
        if((ctrl->flags & V4L2_CTRL_FLAG_DISABLED)) { printf("    Flag: V4L2_CTRL_FLAG_DISABLED.\n"); }
        if((ctrl->flags & V4L2_CTRL_FLAG_GRABBED)) { printf("    Flag: V4L2_CTRL_FLAG_GRABBED.\n"); }
        if((ctrl->flags & V4L2_CTRL_FLAG_READ_ONLY)) { printf("    Flag: V4L2_CTRL_FLAG_READ_ONLY.\n"); }
        if((ctrl->flags & V4L2_CTRL_FLAG_UPDATE)) { printf("    Flag: V4L2_CTRL_FLAG_UPDATE.\n"); }
        if((ctrl->flags & V4L2_CTRL_FLAG_INACTIVE)) { printf("    Flag: V4L2_CTRL_FLAG_INACTIVE.\n"); }
        if((ctrl->flags & V4L2_CTRL_FLAG_SLIDER)) { printf("    Flag: V4L2_CTRL_FLAG_SLIDER.\n"); }
        if((ctrl->flags & V4L2_CTRL_FLAG_WRITE_ONLY)) { printf("    Flag: V4L2_CTRL_FLAG_WRITE_ONLY.\n"); }
        if((ctrl->flags & V4L2_CTRL_FLAG_VOLATILE)) { printf("    Flag: V4L2_CTRL_FLAG_VOLATILE.\n"); }
        if((ctrl->flags & V4L2_CTRL_FLAG_HAS_PAYLOAD)) { printf("    Flag: V4L2_CTRL_FLAG_HAS_PAYLOAD.\n"); }
        if((ctrl->flags & V4L2_CTRL_FLAG_EXECUTE_ON_WRITE)) { printf("    Flag: V4L2_CTRL_FLAG_EXECUTE_ON_WRITE.\n"); }
    }
    //query control's menu
    if ((ctrl->flags & V4L2_CTRL_FLAG_DISABLED) == 0 && ctrl->type == V4L2_CTRL_TYPE_MENU){
        int i2; for(i2 = ctrl->minimum; i2 <= ctrl->maximum; i2++){
            struct v4l2_querymenu mnu;
            memset(&mnu, 0, sizeof(mnu));
            //
            mnu.id = ctrl->id;
            mnu.index = i2;
            //
            int rr3 = v4l2_ioctl(fd, VIDIOC_QUERYMENU, &mnu);
            if(rr3 == 0){
                if(print){
                    if(ctrl->type == V4L2_CTRL_TYPE_INTEGER){
                        printf("    Menu #%d: '%s' = %lld.\n", (i2 + 1), mnu.name, mnu.value);
                    } else {
                        printf("    Menu #%d: '%s'.\n", (i2 + 1), mnu.name);
                    }
                }
            }
        }
    }
    return 0;
}

long msBetweenTimevals(struct timeval* base, struct timeval* next){
    if(base != next){
        if(next == NULL){
            return (base->tv_sec * 1000ULL) + (base->tv_usec / 1000ULL);
        } else if(base == NULL){
            return -((next->tv_sec * 1000ULL) + (next->tv_usec / 1000ULL));
        } else if(base->tv_sec < next->tv_sec || (base->tv_sec == next->tv_sec && base->tv_usec <= next->tv_usec)){
            //ahead
            if(base->tv_sec == next->tv_sec){
                return (next->tv_usec - base->tv_usec) / 1000ULL;
            } else {
                return ((next->tv_sec - base->tv_sec - 1) * 1000ULL) + (((1000000ULL - base->tv_usec) + (next->tv_usec)) / 1000ULL);
            }
        } else {
            //backwards
            if(base->tv_sec == next->tv_sec){
                return -((base->tv_usec - next->tv_usec) / 1000ULL);
            } else {
                return -(((base->tv_sec - next->tv_sec - 1) * 1000ULL) + (((1000000ULL - next->tv_usec) + (base->tv_usec)) / 1000ULL));
            }
        }
    }
    return 0;
}

long msBetweenTimespecs(struct timespec* base, struct timespec* next){
    if(base != next){
        if(next == NULL){
            return (base->tv_sec * 1000ULL) + (base->tv_nsec / 1000000ULL);
        } else if(base == NULL){
            return -((next->tv_sec * 1000ULL) + (next->tv_nsec / 1000000ULL));
        } else if(base->tv_sec < next->tv_sec || (base->tv_sec == next->tv_sec && base->tv_nsec <= next->tv_nsec)){
            //ahead
            if(base->tv_sec == next->tv_sec){
                return (next->tv_nsec - base->tv_nsec) / 1000000ULL;
            } else {
                return ((next->tv_sec - base->tv_sec - 1) * 1000ULL) + (((1000000000ULL - base->tv_nsec) + (next->tv_nsec)) / 1000000ULL);
            }
        } else {
            //backwards
            if(base->tv_sec == next->tv_sec){
                return -((base->tv_nsec - next->tv_nsec) / 1000000ULL);
            } else {
                return -(((base->tv_sec - next->tv_sec - 1) * 1000ULL) + (((1000000000ULL - next->tv_nsec) + (base->tv_nsec)) / 1000000ULL));
            }
        }
    }
    return 0;
}



/*
//ENPlayerPollFdType

typedef enum ENPlayerPollFdType_ {
    ENPlayerPollFdType_Decoder = 0, //dec (decoder).fd
    ENPlayerPollFdType_SrcSocket,   //net.socket
    //
    ENPlayerPollFdType_Count
} ENPlayerPollFdType;

typedef void (*PlayerPollCallback)(void* userParam, struct STPlayer_* plyr, const ENPlayerPollFdType type, int revents);
*/

//STVideoFrameState
//Allows to follow the timings of frames.

void VideoFrameState_init(STVideoFrameState* obj){
    memset(obj, 0, sizeof(*obj));
}

void VideoFrameState_release(STVideoFrameState* obj){
    //nothing
}

//

int VideoFrameState_reset(STVideoFrameState* obj){
    memset(obj, 0, sizeof(*obj));
    return 0;
}

int VideoFrameState_clone(STVideoFrameState* obj, const STVideoFrameState* src){
    //NOTE: update 'VideoFrameState_clone()' if non-static-members are added.
    if(obj != src){
        //static-members' copy
        *obj = *src;
        return 0;
    }
    return -1;
}

void VideoFrameState_iSeqToTimestamp(const unsigned long iSeq, struct timeval* dstTimestamp){
    if(dstTimestamp != NULL){
        dstTimestamp->tv_sec = (iSeq / 1000);
        dstTimestamp->tv_usec = (iSeq % 1000);
    }
}

void VideoFrameState_timestampToSeqIdx(const struct timeval* srcTimestamp, unsigned long *dstSeq){
    if(srcTimestamp != NULL && dstSeq != NULL){
        *dstSeq = (srcTimestamp->tv_sec * 1000) + srcTimestamp->tv_usec;
    }
}


//STVideoFrameStates

void VideoFrameStates_init(STVideoFrameStates* obj){
    memset(obj, 0, sizeof(*obj));
}

void VideoFrameStates_release(STVideoFrameStates* obj){
    if(obj->arr != NULL){
        free(obj->arr);
        obj->arr = NULL;
    }
    obj->use = 0;
    obj->sz = 0;
}

//

int VideoFrameStates_getStateCloningAndRemoveOlder(STVideoFrameStates* obj, const unsigned long iSeq, STVideoFrameState* dstState, int* dstOlderRemovedCount){
    //remove from the right
    int olderRemovedCount = 0;
    while(obj->use > 0){
        STVideoFrameState* st = &obj->arr[obj->use - 1];
        if(st->iSeq > iSeq){ //do not remove newer frames
            break;
        }
        if(st->iSeq == iSeq && dstState != NULL){
            VideoFrameState_clone(dstState, st);
        } else {
            olderRemovedCount++;
        }
        VideoFrameState_release(st);
        obj->use--;
    }
    if(dstOlderRemovedCount != NULL){
        *dstOlderRemovedCount = olderRemovedCount;
    }
    return 0;
}

int VideoFrameStates_addNewestCloning(STVideoFrameStates* obj, const STVideoFrameState* state){
    //increase buffer
    while(obj->sz <= obj->use){
        const int inc = 1; //increment
        STVideoFrameState* arrN = malloc(sizeof(STVideoFrameState) * (obj->sz + inc));
        if(arrN == NULL){
            //alloc fail
            return -1;
        }
        if(obj->arr != NULL){
            if(obj->use > 0){
                memcpy(arrN, obj->arr, sizeof(STVideoFrameState) * obj->use);
            }
            free(obj->arr);
        }
        obj->arr = arrN;
        obj->sz += inc;
    }
    //create gap at the left
    {
        int i; for(i = obj->use; i > 0; i--){
            obj->arr[i] = obj->arr[i - 1];
        }
        obj->use++;
    }
    //add
    {
        STVideoFrameState* st = &obj->arr[0];
        VideoFrameState_init(st);
        VideoFrameState_clone(st, state);
    }
    return 0;
}


//STVideoFrame
//In H264, an Access unit allways produces an output frame.
//IDR = Instantaneous Decoding Refresh

/*
typedef struct STVideoFrame_ {
    STVideoFrameState   state;
    //buff
    struct {
        unsigned char*  ptr;
        int             use;
        int             sz;
    } buff;
} STVideoFrame;
*/

void VideoFrame_init(STVideoFrame* obj){
    memset(obj, 0, sizeof(*obj));
    //
    VideoFrameState_init(&obj->state);
}

void VideoFrame_release(STVideoFrame* obj){
    VideoFrameState_release(&obj->state);
    //buff
    {
        if(obj->buff.ptr != NULL){
            free(obj->buff.ptr);
            obj->buff.ptr = NULL;
        }
        obj->buff.use   = 0;
        obj->buff.sz    = 0;
    }
}
//
int VideoFrame_reset(STVideoFrame* obj){   //state is reseted and buffer 'use' is set to zero
    //
    if(0 != VideoFrameState_reset(&obj->state)){
        printf("VideoFrame_reset, VideoFrameState_reset failed.\n");
        return -1;
    }
    //buff
    {
        obj->buff.use = 0;
    }
    return  0;
}

int VideoFrame_copy(STVideoFrame* obj, const void* data, const int dataSz){
    //increae buffer
    while(obj->buff.sz < (obj->buff.use + dataSz)){
        //create new buffer
        unsigned char* readBuffN = malloc(obj->buff.use + dataSz);
        if(readBuffN == NULL){
            printf("VideoFrame_prepareForFill, malloc failed.\n");
            return -1;
        }
        obj->buff.sz = obj->buff.use + dataSz;
        //old buffer
        if(obj->buff.ptr != NULL){
            //copy
            if(obj->buff.use > 0){
                memcpy(readBuffN, obj->buff.ptr, obj->buff.use);
            }
            //free
            free(obj->buff.ptr);
        }
        obj->buff.ptr = readBuffN;
        //printf("VideoFrame_prepareForFill, buff growth to %dKB.\n", (obj->buff.sz / 1024));
    }
    //copy data
    if(dataSz > 0){
        if(data != NULL){
            memcpy(&obj->buff.ptr[obj->buff.use], data, dataSz);
        }
        obj->buff.use += dataSz;
    }
    return 0;
}

//the buffer is now owned outside the frame, NULLify
int VideoFrame_resignToBuffer(STVideoFrame* obj){
    int r = 0;
    
    return r;
}

//STVideoFrames

/*
typedef struct STVideoFrames_ {
    STVideoFrame**   arr;
    int             sz;
    int             use;
} STVideoFrames;
*/

void VideoFrames_init(STVideoFrames* obj){
    memset(obj, 0, sizeof(*obj));
}

void VideoFrames_release(STVideoFrames* obj){
    if(obj->arr != NULL){
        int i; for(i = 0; i < obj->use; i++){
            STVideoFrame* f = obj->arr[i];
            VideoFrame_release(f);
            free(f);
        }
        free(obj->arr);
        obj->arr = NULL;
    }
    obj->use = 0;
    obj->sz = 0;
}

//get from the right, reuse or creates a new one
int VideoFrames_pullFrameForFill(STVideoFrames* obj, STVideoFrame** dst){
    if(dst != NULL){
        //pull last
        if(obj->use > 0){
            STVideoFrame* f = obj->arr[obj->use - 1];
            //set
            VideoFrame_reset(f);
            f->state.iSeq = obj->iSeqNext++;
            //
            obj->use--;
            *dst = f;
            return 0;
        }
        //create one
        {
            STVideoFrame* f = malloc(sizeof(STVideoFrame));
            VideoFrame_init(f);
            //set
            VideoFrame_reset(f);
            f->state.iSeq = obj->iSeqNext++;
            //
            *dst = f;
            return 0;
        }
    }
    return -1;
}

//get from the left

int VideoFrames_peekFrameForRead(STVideoFrames* obj){ //peek from the left
    return (obj->use > 0 ? 0 : -1);
}

int VideoFrames_pullFrameForRead(STVideoFrames* obj, STVideoFrame** dst){
    if(dst != NULL && obj->use > 0){
        //pull first
        STVideoFrame* f = obj->arr[0];
        //fill gap
        obj->use--;
        {
            int i; for(i = 0; i < obj->use; i++){
                obj->arr[i] = obj->arr[i + 1];
            }
        }
        *dst = f;
        return 0;
    }
    return -1;
}

//add for future pull (reuse)
int VideoFrames_pushFrameOwning(STVideoFrames* obj, STVideoFrame* src){
    //increase buffer
    while(obj->sz <= obj->use){
        const int inc = 1; //increment
        STVideoFrame** arrN = malloc(sizeof(STVideoFrame*) * (obj->sz + inc));
        if(arrN == NULL){
            //alloc fail
            return -1;
        }
        if(obj->arr != NULL){
            if(obj->use > 0){
                memcpy(arrN, obj->arr, sizeof(STVideoFrame*) * obj->use);
            }
            free(obj->arr);
        }
        obj->arr = arrN;
        obj->sz += inc;
    }
    //add
    obj->arr[obj->use] = src;
    obj->use++;
    //
    return 0;
}

//STFbLayoutRect

void FbLayoutRect_init(STFbLayoutRect* obj){
    memset(obj, 0, sizeof(*obj));
}

void FbLayoutRect_release(STFbLayoutRect* obj){
    //nothing
}

//STFbLayoutRow

void FbLayoutRow_init(STFbLayoutRow* obj){
    memset(obj, 0, sizeof(*obj));
}

void FbLayoutRow_release(STFbLayoutRow* obj){
    if(obj->rects != NULL){
        free(obj->rects);
        obj->rects = NULL;
    }
    obj->rectsSz = 0;
    obj->rectsUse = 0;
}

int FbLayoutRow_add(STFbLayoutRow* obj, const int streamId, int x, int y, const int width, const int height){ //will be 'x' ordered automatically
    int r = -1;
    //resize
    while((obj->rectsUse + 1) > obj->rectsSz){
        STFbLayoutRect* arrN = malloc(sizeof(STFbLayoutRect) * (obj->rectsUse + 1));
        if(arrN == NULL){
            printf("Framebuff, layoutSet, malloc failed.\n");
            break;
        }
        obj->rectsSz = (obj->rectsUse + 1);
        if(obj->rects != NULL){
            if(obj->rectsUse > 0){
                memcpy(arrN, obj->rects, sizeof(STFbLayoutRect) * obj->rectsUse);
            }
            free(obj->rects);
            obj->rects = NULL;
        }
        obj->rects = arrN;
    }
    //add record
    if(obj->rectsUse < obj->rectsSz){
        //find location (x ordered)
        STFbLayoutRect* l = &obj->rects[0];
        STFbLayoutRect* lAfterEnd = &obj->rects[obj->rectsUse];
        while(l < lAfterEnd){
            if(l->rect.x < x){
                //next
            } else {
                //move all to the right
                STFbLayoutRect* l2 = lAfterEnd;
                while(l2 > l){
                    *l2 = *(l2 - 1);
                    l2--;
                }
                break;
            }
            l++;
        }
        FbLayoutRect_init(l);
        //
        l->streamId     = streamId;
        l->rect.x       = x;
        l->rect.y       = y;
        l->rect.width   = width;
        l->rect.height  = height;
        //
        obj->rectsUse++;
        //
        if(obj->width < (x + width)){
            obj->width = (x + width);
        }
        if(obj->height < (y + height)){
            obj->height = (y + height);
        }
        //
        r = 0;
    }
    return r;
}

int FbLayoutRow_fillGaps(STFbLayoutRow* obj, const int widthMax){
    if(obj->rectsUse > 0){
        STFbRect first = obj->rects[0].rect;
        STFbRect last = obj->rects[obj->rectsUse - 1].rect;
        //black area on the left
        if(first.x > 0){
            FbLayoutRow_add(obj, 0, 0, 0, first.x, obj->height);
            //printf("FbLayoutRow_fillGaps, added left rect.\n");
        }
        //black area on the right
        if((last.x + last.width) < widthMax){
            FbLayoutRow_add(obj, 0, (last.x + last.width), 0, widthMax - (last.x + last.width), obj->height);
            //printf("FbLayoutRow_fillGaps, added right rect.\n");
        }
    }
    //black areas in current rects
    {
        int i; for(i = (int)obj->rectsUse - 1; i >= 0; i--){ //reverse order because array will grow (ignore new records).
            const STFbRect rect = obj->rects[i].rect; //local copy (array will be modified)
            //black area on the top
            if(rect.y > 0){
                FbLayoutRow_add(obj, 0, rect.x, 0, rect.width, rect.y);
                //printf("FbLayoutRow_fillGaps, added top rect.\n");
            }
            //black area on the bottom
            if((rect.y + rect.height) < obj->height){
                FbLayoutRow_add(obj, 0, rect.x, rect.y + rect.height, rect.width, (obj->height - (rect.y + rect.height)));
                //printf("FbLayoutRow_fillGaps, added bottom rect.\n");
            }
        }
    }
    return 0;
}

//STFramebuffPtr

void FramebuffPtr_init(STFramebuffPtr* obj){
    memset(obj, 0, sizeof(*obj));
    //
    pthread_mutex_init(&obj->mutex, NULL);
    pthread_cond_init(&obj->cond, NULL);
}

void FramebuffPtr_release(STFramebuffPtr* obj){
    pthread_mutex_lock(&obj->mutex);
    {
        K_ASSERT(obj->tasksPendCount == 0);
        while(obj->tasksPendCount > 0){
            pthread_cond_wait(&obj->cond, &obj->mutex);
        }
    }
    pthread_mutex_unlock(&obj->mutex);
    pthread_cond_destroy(&obj->cond);
    pthread_mutex_destroy(&obj->mutex);
    //
    if(obj->ptr != NULL){
        free(obj->ptr);
        obj->ptr = NULL;
    }
    obj->ptrSz = 0;
}


//STFramebuff

void Framebuff_init(STFramebuff* obj){
    memset(obj, 0, sizeof(*obj));
    //
    FramebuffPtr_init(&obj->offscreen);
    FramebuffPtr_init(&obj->screen);
    //
    obj->fd = -1;
}

void Framebuff_release(STFramebuff* obj){
    //cfg
    {
        if(obj->cfg.device != NULL){
            free(obj->cfg.device);
            obj->cfg.device = NULL;
        }
    }
    //
    {
        if(obj->offscreen.ptr != NULL){
            free(obj->offscreen.ptr);
            obj->offscreen.ptr = NULL;
        }
        if(obj->screen.ptr != NULL){
            munmap(obj->screen.ptr, obj->screen.ptrSz);
            obj->screen.ptr = NULL;
        }
        FramebuffPtr_release(&obj->offscreen);
        FramebuffPtr_release(&obj->screen);
    }
    {
        if(obj->blackLine != NULL){
            free(obj->blackLine);
            obj->blackLine = NULL;
        }
        obj->blackLineSz = 0;
    }
    //layout
    {
        //rows
        if(obj->layout.rows.arr != NULL){
            int i; for(i = 0; i < obj->layout.rows.use; i ++){
                STFbLayoutRow* r = &obj->layout.rows.arr[i];
                FbLayoutRow_release(r);
            }
            free(obj->layout.rows.arr);
            obj->layout.rows.arr = NULL;
        }
        obj->layout.rows.use = 0;
        obj->layout.rows.sz = 0;
    }
    //
    if(obj->fd >= 0){
        close(obj->fd);
        obj->fd = -1;
    }
}

//

int Framebuff_open(STFramebuff* obj, const char* device, const int animSecsWaits){
    int r = -1;
    int fd = open(device, O_RDWR);
    if(fd < 0){
        printf("ERROR, Framebuff, open failed: '%s'.\n", device);
    } else {
        //query variables
        struct fb_var_screeninfo vinfo;
        struct fb_fix_screeninfo finfo;
        memset(&vinfo, 0, sizeof(vinfo));
        memset(&finfo, 0, sizeof(finfo));
        if(0 != ioctl(fd, FBIOGET_VSCREENINFO, &vinfo)) {
            printf("ERROR, Framebuff, get variable info failed: '%s'.\n", device);
        } else if(0 != ioctl(fd, FBIOGET_FSCREENINFO, &finfo)) {
            printf("ERROR, Framebuff, get fixed info failed: '%s'.\n", device);
        } else {
            int pixFmt = 0; unsigned char* ptr = NULL; unsigned char* offPtr = NULL;
            //Identify pixFmt
            switch(vinfo.bits_per_pixel){
                case 32:
                    if(
                       vinfo.red.offset == 16 && vinfo.red.length == 8
                       && vinfo.green.offset == 8 && vinfo.green.length == 8
                       && vinfo.blue.offset == 0 && vinfo.blue.length == 8
                       && vinfo.transp.offset == 24 && vinfo.transp.length == 8
                       )
                    {
#                       ifdef V4L2_PIX_FMT_BGR32
                        //deprecated
                        pixFmt = V4L2_PIX_FMT_BGR32;
#                       elif defined(V4L2_PIX_FMT_ABGR32)
                        pixFmt = V4L2_PIX_FMT_ABGR32;
#                       endif
                    } else {
                        //
                    }
                    break;
                case 16:
                    if(
                       vinfo.red.offset == 11 && vinfo.red.length == 5
                       && vinfo.green.offset == 5 && vinfo.green.length == 6
                       && vinfo.blue.offset == 0 && vinfo.blue.length == 5
                       && vinfo.transp.offset == 0 && vinfo.transp.length == 0
                       )
                    {
#                       ifdef V4L2_PIX_FMT_RGB565
                        pixFmt = V4L2_PIX_FMT_RGB565; //tmp
#                       elif def V4L2_PIX_FMT_RGB565X
                        pixFmt = V4L2_PIX_FMT_RGB565X;
#                       endif
                    } else {
                        //
                    }
                    break;
                default:
                    break;
            }
            printf("Framebuff, opened: '%s'.\n", device);
            printf("Framebuff, fixed info:\n");
            printf("           smem_start: %u.\n", finfo.smem_start);
            printf("             smem_len: %u.\n", finfo.smem_len);
            printf("                 type: %u ('%s').\n", finfo.type, finfo.type == FB_TYPE_PACKED_PIXELS ? "FB_TYPE_PACKED_PIXELS" : finfo.type == FB_TYPE_PLANES ? "FB_TYPE_PLANES" : finfo.type == FB_TYPE_INTERLEAVED_PLANES ? "FB_TYPE_INTERLEAVED_PLANES" : finfo.type == FB_TYPE_FOURCC ? "FB_TYPE_FOURCC" : "UNKNOWN_STR");
            printf("             type_aux: %u ('%s').\n", finfo.type_aux, finfo.type_aux == FB_TYPE_PACKED_PIXELS ? "FB_TYPE_PACKED_PIXELS" : finfo.type_aux == FB_TYPE_PLANES ? "FB_TYPE_PLANES" : finfo.type_aux == FB_TYPE_INTERLEAVED_PLANES ? "FB_TYPE_INTERLEAVED_PLANES" : finfo.type_aux == FB_TYPE_FOURCC ? "FB_TYPE_FOURCC" : "UNKNOWN_STR");
            printf("               visual: %u ('%s').\n", finfo.visual, finfo.visual == FB_VISUAL_MONO01 ? "FB_VISUAL_MONO01" : finfo.visual == FB_VISUAL_MONO10 ? "FB_VISUAL_MONO10" : finfo.visual == FB_VISUAL_TRUECOLOR ? "FB_VISUAL_TRUECOLOR" : finfo.visual == FB_VISUAL_PSEUDOCOLOR ? "FB_VISUAL_PSEUDOCOLOR" : finfo.visual == FB_VISUAL_STATIC_PSEUDOCOLOR ? "FB_VISUAL_STATIC_PSEUDOCOLOR" : finfo.visual == FB_VISUAL_DIRECTCOLOR ? "FB_VISUAL_DIRECTCOLOR" : finfo.visual == FB_VISUAL_FOURCC ? "FB_VISUAL_FOURCC" : "UNKNOWN_STR");
            printf("             xpanstep: %u.\n", finfo.xpanstep);
            printf("             ypanstep: %u.\n", finfo.ypanstep);
            printf("            ywrapstep: %u.\n", finfo.ywrapstep);
            printf("          line_length: %u.\n", finfo.line_length);
            printf("           mmio_start: %u.\n", finfo.mmio_start);
            printf("             mmio_len: %u.\n", finfo.mmio_len);
            printf("                accel: %u.\n", finfo.accel);
            printf("         capabilities: %u%s.\n", finfo.capabilities, (finfo.capabilities & FB_CAP_FOURCC) ? " FB_CAP_FOURCC" : "");
            //
            printf("Framebuff, variable info:\n");
            printf("                 xres: %u.\n", vinfo.xres);
            printf("                 yres: %u.\n", vinfo.yres);
            printf("         xres_virtual: %u.\n", vinfo.xres_virtual);
            printf("         yres_virtual: %u.\n", vinfo.yres_virtual);
            printf("              xoffset: %u.\n", vinfo.xoffset);
            printf("              yoffset: %u.\n", vinfo.yoffset);
            printf("       bits_per_pixel: %u.\n", vinfo.bits_per_pixel);
            printf("            grayscale: %u (%s).\n", vinfo.grayscale, vinfo.grayscale == 0 ? "COLOR" : vinfo.grayscale == 1 ? "GRAYSCALE" : "FOURCC");
            printf("                  red: %u, +%u, %s.\n", vinfo.red.offset, vinfo.red.length, vinfo.red.msb_right ? "msb_right" : "msb_left");
            printf("                green: %u, +%u, %s.\n", vinfo.green.offset, vinfo.green.length, vinfo.green.msb_right ? "msb_right" : "msb_left");
            printf("                 blue: %u, +%u, %s.\n", vinfo.blue.offset, vinfo.blue.length, vinfo.blue.msb_right ? "msb_right" : "msb_left");
            printf("               transp: %u, +%u, %s.\n", vinfo.transp.offset, vinfo.transp.length, vinfo.transp.msb_right ? "msb_right" : "msb_left");
            printf("               nonstd: %u.\n", vinfo.nonstd);
            printf("             activate: %u.\n", vinfo.activate);
            printf("               height: %u mm.\n", vinfo.height);
            printf("                width: %u mm.\n", vinfo.width);
            printf("             pixclock: %u pico-secs.\n", vinfo.pixclock);
            printf("          left_margin: %u pixclocks.\n", vinfo.left_margin);
            printf("         right_margin: %u pixclocks.\n", vinfo.right_margin);
            printf("         upper_margin: %u pixclocks.\n", vinfo.upper_margin);
            printf("         lower_margin: %u pixclocks.\n", vinfo.lower_margin);
            printf("            hsync_len: %u pixclocks.\n", vinfo.hsync_len);
            printf("            vsync_len: %u pixclocks.\n", vinfo.vsync_len);
            printf("                 sync: %u.\n", vinfo.sync);
            printf("                vmode: %u.\n", vinfo.vmode);
            printf("               rotate: %u deg.\n", vinfo.rotate);
            printf("           colorspace: %u.\n", vinfo.colorspace);
            //
            if(pixFmt == 0){
                printf("ERROR, Framebuff, unsupported pixfmt: '%s' (add this case to source code!).\n", device);
            } else if(MAP_FAILED == (ptr = (unsigned char*)mmap(0, finfo.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0))){
                printf("ERROR, Framebuff, mmap failed.\n", device);
            } else if(!(offPtr = malloc(finfo.smem_len))){
                printf("ERROR, Framebuff, malloc for offscreen buffer failed.\n", device);
            } else {
                if(obj->offscreen.ptr != NULL){
                    free(obj->offscreen.ptr);
                    obj->offscreen.ptr = NULL;
                }
                if(obj->screen.ptr != NULL){
                    munmap(obj->screen.ptr, obj->screen.ptrSz);
                    obj->screen.ptr = NULL;
                }
                if(obj->fd >= 0){
                    close(obj->fd);
                    obj->fd = -1;
                }
                //cfg
                {
                    if(device != obj->cfg.device){
                        if(obj->cfg.device != NULL){ free(obj->cfg.device); obj->cfg.device = NULL; }
                        if(device != NULL){
                            int deviceLen = strlen(device);
                            obj->cfg.device = malloc(deviceLen + 1);
                            memcpy(obj->cfg.device, device, deviceLen + 1);
                        }
                    }
                    //
                    obj->cfg.animSecsWaits = animSecsWaits;
                }
                //offscreen
                obj->offscreen.ptr = offPtr; offPtr = NULL; //consume
                obj->offscreen.ptrSz = finfo.smem_len;
                //screen
                obj->screen.ptr    = ptr; ptr = NULL; //consume
                obj->screen.ptrSz  = finfo.smem_len;
                //
                obj->fd     = fd; fd = -1; //consume
                obj->vinfo  = vinfo;
                obj->finfo  = finfo;
                obj->pixFmt = pixFmt;
                obj->bitsPerPx = vinfo.bits_per_pixel;
                obj->bytesPerLn = finfo.line_length;
                obj->width  = vinfo.xres;
                obj->height = vinfo.yres;
                //
                if(obj->blackLineSz != obj->bytesPerLn){
                    {
                        if(obj->blackLine != NULL){
                            free(obj->blackLine);
                            obj->blackLine = NULL;
                        }
                        obj->blackLineSz = 0;
                    }
                    if(obj->bytesPerLn > 0){
                        obj->blackLine = malloc(obj->bytesPerLn);
                        if(obj->blackLine != NULL){
                            obj->blackLineSz = obj->bytesPerLn;
                        }
                    }
                }
                //
                r = 0;
            }
            //release (if not conusmed)
            if(offPtr != NULL){
                free(offPtr);
                offPtr = NULL;
            }
            //unmap (if not consumed)
            if(ptr != NULL && ptr != MAP_FAILED){
                munmap(ptr, finfo.smem_len);
                ptr = NULL;
            }
        }
        //close (if not consumed)
        if(fd >= 0){
            close(fd);
            fd = -1;
        }
    }
    return r;
}

int Framebuff_validateRect(STFramebuff* obj, STFbPos pPos, STFbPos* dstPos, STFbRect pSrcRect, STFbRect* dstRect){
    //only full-bytes are allowed
    STFbPos pos = pPos;
    STFbRect srcRect = pSrcRect;
    //Validate negative rect
    {
        if(srcRect.width < 0){
            srcRect.x        += srcRect.width;
            srcRect.width    = -srcRect.width;
        }
        if(srcRect.height < 0){
            srcRect.y        += srcRect.height;
            srcRect.height    = -srcRect.height;
        }
    } K_ASSERT(srcRect.width >= 0 && srcRect.height >= 0)
    //Validate negative start
    {
        if(pos.x < 0){
            srcRect.x        -= pos.x;
            srcRect.width    = (pos.x <= -srcRect.width ? 0 : srcRect.width + pos.x);
            pos.x            = 0;
        }
        if(pos.y < 0){
            srcRect.y        -= pos.y;
            srcRect.height   = (pos.y <= -srcRect.height ? 0 : srcRect.height + pos.y);
            pos.y            = 0;
        }
    } K_ASSERT(srcRect.x >= 0 && srcRect.y >= 0 && srcRect.width >= 0 && srcRect.height >= 0)
    //Validate against dst-rect
    {
        if((pos.x + srcRect.width) > obj->width){
            srcRect.width = (obj->width - pos.x);
        }
        if((pos.y + srcRect.height) > obj->height){
            srcRect.height = (obj->height - pos.y);
        }
    }
    //Copy content
    if(dstPos != NULL){
        *dstPos = pos;
    }
    if(dstRect != NULL){
        *dstRect = srcRect;
    }
    return 0;
}

int Framebuff_bitblit(STFramebuff* obj, STFramebuffPtr* dst, STFbPos pos, const struct STPlane_* srcPixs, STFbRect srcRect){
    int r = -1;
    //only full-bytes are allowed
    if(0 != Framebuff_validateRect(obj, pos, &pos, srcRect, &srcRect)){
        //
    } else if(dst != NULL && dst->ptr != NULL && dst->ptrSz > 0){
        //Copy content
        if(srcRect.width <= 0 || srcRect.height <= 0){
            //PRINTF_INFO("Bitmaqp omited.\n");
        } else {
            //Validate, inside-dstRange
            K_ASSERT(pos.x >= 0 && pos.x < obj->width)
            K_ASSERT(pos.y >= 0 && pos.y < obj->height)
            K_ASSERT((pos.x + srcRect.width) > 0 && (pos.x + srcRect.width) <= obj->width)
            K_ASSERT((pos.y + srcRect.height) > 0 && (pos.y + srcRect.height) <= obj->height)
            //ToDo: implement fast-copy when complete dst-lines can will be filled
            const int bytesPerPx = (obj->bitsPerPx / 8);
            int y = srcRect.y, yAfterEnd = srcRect.y + srcRect.height;
            if((srcPixs->bytesPerLn % 4) != 0){
                printf("ERROR, Framebuff, bitblit, src bytesPerLn is not 32-bits-aligned.\n");
            } else if((obj->bytesPerLn % 4) != 0){
                printf("ERROR, Framebuff, bitblit, buffer bytesPerLn is not 32-bits-aligned.\n");
            } else {
                while(y < yAfterEnd){
                    unsigned char* srcLn = &srcPixs->dataPtr[(srcPixs->bytesPerLn * y) + (bytesPerPx * srcRect.x)];
                    unsigned char* dstLn = &dst->ptr[(obj->bytesPerLn * pos.y) + (bytesPerPx * pos.x)];
                    int copyLen = bytesPerPx * srcRect.width;
                    if(copyLen > 0){
                        memcpy(dstLn, srcLn, copyLen);
                    }
                    y++; pos.y++;
                }
            }
        }
        r = 0;
    }
    return r;
}

//resets 'isFound'
int Framebuff_layoutStart(STFramebuff* obj){
    //remove current rows
    {
        int i; for(i = 0 ; i < obj->layout.rows.use; i++){
            STFbLayoutRow* r = &obj->layout.rows.arr[i];
            FbLayoutRow_release(r);
        }
        obj->layout.rows.use = 0;
        obj->layout.rows.rectsCount = 0;
        obj->layout.width = 0;
        obj->layout.height = 0;
    }
    //open first row
    {
        //resize array
        while((obj->layout.rows.use + 1) > obj->layout.rows.sz){
            STFbLayoutRow* arrN = malloc(sizeof(STFbLayoutRow) * (obj->layout.rows.use + 1));
            if(arrN == NULL){
                break;
            } else {
                if(obj->layout.rows.arr != NULL){
                    if(obj->layout.rows.use > 0){
                        memcpy(arrN, obj->layout.rows.arr, sizeof(STFbLayoutRow) * obj->layout.rows.use);
                    }
                    free(obj->layout.rows.arr);
                    obj->layout.rows.arr = NULL;
                }
                obj->layout.rows.arr = arrN;
                obj->layout.rows.sz = (obj->layout.rows.use + 1);
            }
        }
        //add
        if(obj->layout.rows.use < obj->layout.rows.sz){
            STFbLayoutRow* r = &obj->layout.rows.arr[obj->layout.rows.use];
            FbLayoutRow_init(r);
            obj->layout.rows.use++;
        }
    }
    return 0;
}

//removes records without 'isFound' flag, and organizes in order
int Framebuff_layoutEnd(STFramebuff* obj){
    if(obj->layout.rows.use > 0){
        STFbLayoutRow* row = &obj->layout.rows.arr[obj->layout.rows.use - 1];
        if(row->rectsUse == 0){
            //remove empty row
            FbLayoutRow_release(row);
            obj->layout.rows.use--;
        } else {
            //close row
            FbLayoutRow_fillGaps(row, obj->width);
            obj->layout.rows.rectsCount += row->rectsUse;
            if(obj->layout.width < row->width) obj->layout.width = row->width;
            if(obj->layout.height < (row->yTop + row->height)) obj->layout.height = (row->yTop + row->height);
        }
    }
    return 0;
}

//updates or add the stream, flags it as 'isFound'
int Framebuff_layoutAdd(STFramebuff* obj, int streamId, const STFbSize size){
    int r = -1;
    //current row
    if(obj->layout.rows.use > 0){
        STFbLayoutRow* row = &obj->layout.rows.arr[obj->layout.rows.use - 1];
        if(row->rectsUse == 0 || (row->width + size.width) <= obj->width){
            //add
            FbLayoutRow_add(row, streamId, row->width, 0, size.width, size.height);
        } else {
            //close previous row
            FbLayoutRow_fillGaps(row, obj->width);
            obj->layout.rows.rectsCount += row->rectsUse;
            if(obj->layout.width < row->width) obj->layout.width = row->width;
            if(obj->layout.height < (row->yTop + row->height)) obj->layout.height = (row->yTop + row->height);
            //open next row
            {
                int nextY = row->yTop + row->height;
                //resize array
                while((obj->layout.rows.use + 1) > obj->layout.rows.sz){
                    STFbLayoutRow* arrN = malloc(sizeof(STFbLayoutRow) * (obj->layout.rows.use + 1));
                    if(arrN == NULL){
                        break;
                    } else {
                        if(obj->layout.rows.arr != NULL){
                            if(obj->layout.rows.use > 0){
                                memcpy(arrN, obj->layout.rows.arr, sizeof(STFbLayoutRow) * obj->layout.rows.use);
                            }
                            free(obj->layout.rows.arr);
                            obj->layout.rows.arr = NULL;
                        }
                        obj->layout.rows.arr = arrN;
                        obj->layout.rows.sz = (obj->layout.rows.use + 1);
                    }
                }
                //add
                if(obj->layout.rows.use < obj->layout.rows.sz){
                    STFbLayoutRow* row = &obj->layout.rows.arr[obj->layout.rows.use];
                    FbLayoutRow_init(row);
                    obj->layout.rows.use++;
                    //
                    row->yTop = nextY;
                    FbLayoutRow_add(row, streamId, row->width, 0, size.width, size.height);
                }
            }
        }
    }
    r = 0;
    return r;
}

//find this stream current location in scene
int Framebuff_streamFindStreamId(STFramebuff* obj, int streamId){
    int r = -1;
    //find stream and set size
    int i, i2; for(i = (int)obj->layout.rows.use - 1; i >= 0 && r != 0; i--){
        STFbLayoutRow* row = &obj->layout.rows.arr[i];
        for(i2 = (int)row->rectsUse - 1; i2 >= 0; i2--){
            if(row->rects[i2].streamId == streamId){
                r = 0;
                break;
            }
        }
    }
    return r;
}

int Framebuff_animTick(STFramebuff* obj, int ms){
    int r = 0;
    //layout
    if(ms > 0){
        //wait
        {
            if(obj->layout.anim.msWait <= ms){
                obj->layout.anim.msWait = 0;
            } else {
                obj->layout.anim.msWait -= ms;
            }
        }
        //move
        if(obj->layout.anim.msWait <= 0){
            //row animation
            if(obj->layout.rows.use <= 0 || obj->layout.height <= 0){
                obj->layout.anim.iRowFirst  = 0;
                obj->layout.anim.yOffset    = 0;
                obj->layout.anim.msWait     = (obj->cfg.animSecsWaits * 1000);
                //flag to refresh
                obj->offscreen.isSynced     = 0;
            } else {
                //move rows
                STFbLayoutRow* row          = &obj->layout.rows.arr[obj->layout.anim.iRowFirst % obj->layout.rows.use];
                int pxMoveV = ((obj->height * 50 / 100) * ms / 1000); //50% per sec
                if(pxMoveV <= 0) pxMoveV = 1;
                {
                    int yOffsetDst              = -(row->yTop);
                    while(yOffsetDst > obj->layout.anim.yOffset) yOffsetDst -= obj->layout.height;
                    //move
                    obj->layout.anim.yOffset -= pxMoveV;
                    if(obj->layout.anim.yOffset <= yOffsetDst){
                        obj->layout.anim.yOffset = yOffsetDst;
                    }
                    //move to next row
                    if(obj->layout.anim.yOffset == yOffsetDst){
                        obj->layout.anim.yOffset    = (yOffsetDst % obj->layout.height);
                        obj->layout.anim.iRowFirst  = (obj->layout.anim.iRowFirst + 1) % obj->layout.rows.use;
                        obj->layout.anim.msWait     = (obj->cfg.animSecsWaits * 1000);
                        //flag to refresh
                        obj->offscreen.isSynced     = 0;
                    }
                }
            }
        }
    }
    return r;
}

//---------------------
//-- Unplanned drawing
//---------------------
//Draws the rects in order. This means the src and dst memory area will jump.

//STFramebuffDrawTaskUnplaned

typedef struct STFramebuffDrawTaskUnplaned_ {
    STFramebuff* obj;
    struct STPlayer_* plyr;
    STFramebuffPtr* dst;
    //
    STFbLayoutRect* l;
    STStreamContext* ctx;
    STPlane* p;
    //
    STFbPos pos;
    STFbRect srcRect;
} STFramebuffDrawTaskUnplaned;

void Framebuff_drawUnplanedTaskFunc_(void* param){
    STFramebuffDrawTaskUnplaned* t = (STFramebuffDrawTaskUnplaned*)param;
    //draw
    {
        if(0 != Framebuff_bitblit(t->obj, t->dst, t->pos, t->p, t->srcRect)){
            printf("ERROR, StreamContext, bitblit failed.\n");
        }
    }
    //reduce counter
    {
        pthread_mutex_lock(&t->dst->mutex);
        {
            K_ASSERT(t->dst->tasksPendCount > 0)
            if(t->dst->tasksPendCount > 0){
                t->dst->tasksPendCount--;
                if(t->dst->tasksPendCount == 0){ //optimization, only awake main-thread if is the last task.
                    pthread_cond_broadcast(&t->dst->cond);
                }
            }
        }
        pthread_mutex_unlock(&t->dst->mutex);
    }
}

int Framebuff_drawToPtrUnplanned(STFramebuff* obj, struct STPlayer_* plyr, STFramebuffPtr* dst){
    int r = -1;
    if(plyr != NULL){
        r = 0;
        /*int tasksSz = 0;
        //count all streams
        int i; for(i = 0; i < obj->layout.use; i++){
            STFbLayoutRect* l = &obj->layout.arr[i];
            int j; for(j = 0; j < plyr->streams.arrUse; j++){
                STStreamContext* ctx = plyr->streams.arr[j];
                if(ctx->streamId == l->streamId){
                    //render last buffer
                    STBuffer* buff = (ctx->dec.dst.isLastDequeuedCloned ? &ctx->dec.dst.lastDequeuedClone : ctx->dec.dst.lastDequeued);
                    if(buff == NULL){
                        //not available
                        //ToDo: render black or text
                    } else {
                        STPlane* p = &buff->planes[0];
                        //draw to fbs (all)
                        //Test results:
                        //Single thread:
                        //(1) drawing fullscreen exact size of fb (1824 x 984): 17% cpu.
                        //(2) drawing fullscreen smaller image (640 x 480) than fb (1824 x 984): 10% cpu.
                        //(3) drawing fullscreen bigger image(1920 x 1088) than fb (1824 x 984): 40% cpu.
                        {
                            STFbRect layoutRect = l->rect;
                            STFbPos pos;
                            STFbRect srcRect;
                            memset(&pos, 0, sizeof(pos));
                            memset(&srcRect, 0, sizeof(srcRect));
                            //
                            pos.x = obj->layout.anim.xOffset + layoutRect.x;
                            pos.y = obj->layout.anim.yOffset + layoutRect.y;
                            //
                            srcRect         = ctx->dec.dst.composition;
                            //
                            //srcRect.x       = 0;    //ToDo: center
                            //srcRect.y       = 0;    //ToDo: center
                            //srcRect.width   = (layoutRect.width < ctx->dec.dst.width ? layoutRect.width : ctx->dec.dst.width);
                            //srcRect.height  = (layoutRect.height < ctx->dec.dst.height ? layoutRect.height : ctx->dec.dst.height);
                            if(0 != Framebuff_validateRect(obj, pos, &pos, srcRect, &srcRect)){
                                printf("ERROR, StreamContext, bitblit failed.\n");
                            } else if(srcRect.width > 0 && srcRect.height > 0){
                                tasksSz++;
                            }
                        }
                    }
                    break;
                }
            }
        }
        if(tasksSz > 0){
            int tasksUse = 0;
            STFramebuffDrawTaskUnplaned* tt = malloc(sizeof(STFramebuffDrawTaskUnplaned) * tasksSz);
            if(tt != NULL){
                //add tasks
                {
                    int i; for(i = 0; i < obj->layout.use; i++){
                        STFbLayoutRect* l = &obj->layout.arr[i];
                        int j; for(j = 0; j < plyr->streams.arrUse; j++){
                            STStreamContext* ctx = plyr->streams.arr[j];
                            if(ctx->streamId == l->streamId){
                                //render last buffer
                                STBuffer* buff = (ctx->dec.dst.isLastDequeuedCloned ? &ctx->dec.dst.lastDequeuedClone : ctx->dec.dst.lastDequeued);
                                if(buff == NULL){
                                    //not available
                                    //ToDo: render black or text
                                } else {
                                    STPlane* p = &buff->planes[0];
                                    //draw to fbs (all)
                                    //Test results:
                                    //Single thread:
                                    //(1) drawing fullscreen exact size of fb (1824 x 984): 17% cpu.
                                    //(2) drawing fullscreen smaller image (640 x 480) than fb (1824 x 984): 10% cpu.
                                    //(3) drawing fullscreen bigger image(1920 x 1088) than fb (1824 x 984): 40% cpu.
                                    {
                                        STFbRect layoutRect = l->rect;
                                        STFbPos pos;
                                        STFbRect srcRect;
                                        memset(&pos, 0, sizeof(pos));
                                        memset(&srcRect, 0, sizeof(srcRect));
                                        //
                                        pos.x = obj->layout.anim.xOffset + layoutRect.x;
                                        pos.y = obj->layout.anim.yOffset + layoutRect.y;
                                        //
                                        srcRect         = ctx->dec.dst.composition;
                                        //
                                        //srcRect.x       = 0;    //ToDo: center
                                        //srcRect.y       = 0;    //ToDo: center
                                        //srcRect.width   = (layoutRect.width < ctx->dec.dst.width ? layoutRect.width : ctx->dec.dst.width);
                                        //srcRect.height  = (layoutRect.height < ctx->dec.dst.height ? layoutRect.height : ctx->dec.dst.height);
                                        if(0 != Framebuff_validateRect(obj, pos, &pos, srcRect, &srcRect)){
                                            printf("ERROR, StreamContext, bitblit failed.\n");
                                        } else if(srcRect.width > 0 && srcRect.height > 0){
                                            if(tasksUse < tasksSz){
                                                STFramebuffDrawTaskUnplaned* t = &tt[tasksUse];
                                                memset(t, 0, sizeof(*t));
                                                //
                                                t->obj      = obj;
                                                t->plyr     = plyr;
                                                t->dst      = dst;
                                                //
                                                t->l        = l;
                                                t->ctx      = ctx;
                                                t->p        = p;
                                                //
                                                t->pos      = pos;
                                                t->srcRect  = srcRect;
                                                //
                                                tasksUse++;
                                            }
                                        }
                                    }
                                }
                                break;
                            }
                        }
                    }
                    //execute tasks
                    {
                        int i; for(i = 0 ; i < tasksUse; i++){
                            int taskIsRunning = 0;
                            STFramebuffDrawTaskUnplaned* task = &tt[i];
                            const int iThread = 0; //i * (plyr->threads.use + 1) / tasksUse; // +1 is this thread
                            /*if(iThread < plyr->threads.use){
                                //execute in helper thread
                                STThread* thread = &plyr->threads.arr[iThread];
                                pthread_mutex_lock(&dst->mutex);
                                {
                                    dst->tasksPendCount++;
                                }
                                pthread_mutex_unlock(&dst->mutex);
                                if(0 != Thread_addTask(thread, Framebuff_drawUnplanedTaskFunc_, task)){
                                    //printf("ERROR, Thread_addTask failed.\n"); //ToDo: comment this print (risk to noisy).
                                    pthread_mutex_lock(&dst->mutex);
                                    {
                                        dst->tasksPendCount--;
                                    }
                                    pthread_mutex_unlock(&dst->mutex);
                                } else {
                                    taskIsRunning = 1;
                                }
                            }* /
                            //execute in this thread
                            if(!taskIsRunning){
                                pthread_mutex_lock(&dst->mutex);
                                {
                                    dst->tasksPendCount++;
                                }
                                pthread_mutex_unlock(&dst->mutex);
                                Framebuff_drawUnplanedTaskFunc_(task);
                            }
                        }
                    }
                }
                //wait for tasks
                {
                    pthread_mutex_lock(&dst->mutex);
                    while(dst->tasksPendCount > 0){
                        K_ASSERT(dst->tasksPendCount >= 0)
                        pthread_cond_wait(&dst->cond, &dst->mutex);
                    }
                    pthread_mutex_unlock(&dst->mutex);
                }
                free(tt);
                tt = NULL;
            }
        }*/
        //
        obj->screen.isSynced = 0;
    }
    return r;
}

//---------------------
//-- Planned drawing
//---------------------
//Draws the lines in order. This means the dst memory area will be sequential, the src memory area will be jumping.


//STFramebuffDrawLine

//All the lines to be drawn, in dst-pointer order.
//The purpose is to organize all the memcpy instructions
//reducing the memory-jumps, to make the memcpy as closer
//as posible to as one single call for the dst-memory area.

//One line of a image, will be memcopied.
typedef struct STFramebuffDrawLine_ {
    void*   dst;
    void*   src;
    int     sz;
} STFramebuffDrawLine;

//STFramebuffDrawRect

typedef struct STFramebuffDrawRect_ {
    int         iRow;
    STPlane*    plane;      //bitmap
    STFbPos     posCur;     //current dst position (y will be updated)
    int         srcRectX;   //
    int         srcRectWidth;   //
    int         srcRectY;   //will be updated
    int         srcRectYAfterEnd;
} STFramebuffDrawRect;

void Framebuff_drawToPtrPlannedBuildRowPlan_(STFramebuff* obj, struct STPlayer_* plyr, STFramebuffPtr* dst, STFramebuffDrawRect* rects, const int rectsUse, STFramebuffDrawLine* lines, int* linesUse){
    if(rects != NULL && rectsUse > 0){
        STFramebuffDrawRect* rect = NULL;
        const STFramebuffDrawRect* rectAfterEnd = rects + rectsUse;
        const int bytesPerPx = (obj->bitsPerPx / 8);
        int yDest = rects[0].posCur.y;
        int lnFound = 0;
        do {
            rect = rects; lnFound = 0;
            while(rect < rectAfterEnd){
                if(rect->posCur.y == yDest && rect->srcRectY < rect->srcRectYAfterEnd){
                    //add line
                    STFramebuffDrawLine* ln = &lines[*linesUse];
                    ln->dst = &dst->ptr[(obj->bytesPerLn * rect->posCur.y) + (bytesPerPx * rect->posCur.x)];
                    if(rect->plane != NULL){
                        ln->src = &rect->plane->dataPtr[(rect->plane->bytesPerLn * rect->srcRectY) + (bytesPerPx * rect->srcRectX)];
                    } else {
                        ln->src = obj->blackLine;
                    }
                    ln->sz  = bytesPerPx * rect->srcRectWidth;
                    (*linesUse)++;
                    lnFound= 1;
                    //next line
                    rect->posCur.y++;
                    rect->srcRectY++;
                }
                //next rect
                rect++;
            }
            //next row
            yDest++;
        } while(lnFound);
    }
}

//STFramebuffDrawTaskUnplaned

typedef struct STFramebuffDrawTaskPlaned_ {
    STFramebuffPtr*     dst;
    STFramebuffDrawLine* lines;
    int                  linesSz;
} STFramebuffDrawTaskPlaned;

void Framebuff_drawPplanedTaskFunc_(void* param){
    STFramebuffDrawTaskPlaned* t = (STFramebuffDrawTaskPlaned*)param;
    //draw
    {
        STFramebuffDrawLine* ln = t->lines;
        STFramebuffDrawLine* lnAfterEnd = ln + t->linesSz;
        while(ln < lnAfterEnd){
            if(ln->sz > 0){
                memcpy(ln->dst, ln->src, ln->sz);
            }
            //next
            ln++;
        }
    }
    //reduce counter
    {
        pthread_mutex_lock(&t->dst->mutex);
        {
            K_ASSERT(t->dst->tasksPendCount > 0)
            if(t->dst->tasksPendCount > 0){
                t->dst->tasksPendCount--;
                if(t->dst->tasksPendCount == 0){ //optimization, only awake main-thread if is the last task.
                    pthread_cond_broadcast(&t->dst->cond);
                }
            }
        }
        pthread_mutex_unlock(&t->dst->mutex);
    }
}

int Framebuff_drawToPtrPlanned(STFramebuff* obj, struct STPlayer_* plyr, STFramebuffPtr* dst){
    int r = -1;
    if(plyr != NULL){
        r = 0;
        //printf("Drawing planned: %d rows, %d rects.\n", obj->layout.rows.use, obj->layout.rows.rectsCount);
        if(obj->layout.rows.use > 0 && obj->layout.rows.rectsCount > 0){
            int rectsUse = 0, linesSz = 0, rectsSz = (obj->layout.rows.rectsCount * 2); //*2 just to be able to draw screen area twice (scroll down)
            STFramebuffDrawRect* rects = malloc(sizeof(STFramebuffDrawRect) * rectsSz);
            int rowsPassedCount = 0;
            //add all rects to be drawn
            if(rects != NULL){
                memset(rects, 0, sizeof(sizeof(STFramebuffDrawRect) * rectsSz));
                //validate 'iRowFirst' value range.
                int yTop = obj->layout.anim.yOffset;
                //printf("Draw planned, yOffset(%d).\n", obj->layout.anim.yOffset);
                while(1){
                    int yTopBefore = yTop;
                    int i2; for(i2 = 0; i2 < obj->layout.rows.use; i2++){
                        STFbLayoutRow* row = &obj->layout.rows.arr[i2];
                        int rowXPrev = -1, yTopPrev = yTop;
                        int i; for(i = 0; i < row->rectsUse; i++){
                            STFbLayoutRect* lyRect = &row->rects[i];
                            int rectAdded = 0;
                            K_ASSERT(rowXPrev <= lyRect->rect.x);
                            rowXPrev = lyRect->rect.x;
                            //add stream
                            if(lyRect->streamId > 0 && rectsUse < rectsSz){
                                int j; for(j = 0; j < plyr->streams.arrUse; j++){
                                    STStreamContext* ctx = plyr->streams.arr[j];
                                    if(ctx->streamId == lyRect->streamId){
                                        if(ctx->dec.dst.pixelformat == obj->pixFmt){
                                            //render last buffer
                                            STBuffer* buff = (ctx->dec.dst.isLastDequeuedCloned ? &ctx->dec.dst.lastDequeuedClone : ctx->dec.dst.lastDequeued);
                                            if(buff == NULL){
                                                //not available
                                                //ToDo: render black or text
                                            } else {
                                                STPlane* p = &buff->planes[0];
                                                //draw to fbs (all)
                                                //Test results:
                                                //Single thread:
                                                //(1) drawing fullscreen exact size of fb (1824 x 984): 17% cpu.
                                                //(2) drawing fullscreen smaller image (640 x 480) than fb (1824 x 984): 10% cpu.
                                                //(3) drawing fullscreen bigger image(1920 x 1088) than fb (1824 x 984): 40% cpu.
                                                STFbRect srcRect = ctx->dec.dst.composition;
                                                STFbPos pos;
                                                pos.x = lyRect->rect.x;
                                                pos.y = yTop + lyRect->rect.y;
                                                //
                                                if(0 != Framebuff_validateRect(obj, pos, &pos, srcRect, &srcRect)){
                                                    printf("ERROR, StreamContext, bitblit failed.\n");
                                                } else if(srcRect.width > 0 && srcRect.height > 0){
                                                    STFramebuffDrawRect* rect = &rects[rectsUse];
                                                    //
                                                    //printf("Stream-rect-added row(%d) pos(%d, %d) srcRect(%d, %d)-(+%d, +%d).\n", iRow, pos.x, pos.y, srcRect.x, srcRect.y, srcRect.width, srcRect.height);
                                                    //
                                                    rect->iRow      = rowsPassedCount;
                                                    rect->plane     = p;
                                                    rect->posCur    = pos;
                                                    rect->srcRectX  = srcRect.x;
                                                    rect->srcRectWidth = srcRect.width;
                                                    rect->srcRectY  = srcRect.y;
                                                    rect->srcRectYAfterEnd = srcRect.y + srcRect.height;
                                                    //
                                                    rectAdded = 1;
                                                    rectsUse++;
                                                    linesSz += srcRect.height;
                                                } else {
                                                    //printf("Stream-rect-ignored row(%d) pos(%d, %d) srcRect(%d, %d)-(+%d, +%d).\n", iRow, pos.x, pos.y, srcRect.x, srcRect.y, srcRect.width, srcRect.height);
                                                }
                                            }
                                        }
                                        break;
                                    }
                                }
                            }
                            //add black rect
                            if(!rectAdded && rectsUse < rectsSz){
                                STFbRect srcRect;
                                STFbPos pos;
                                pos.x = lyRect->rect.x;
                                pos.y = yTop + lyRect->rect.y;
                                srcRect.x = 0;
                                srcRect.y = 0;
                                srcRect.width = lyRect->rect.width;
                                srcRect.height = lyRect->rect.height;
                                if(0 != Framebuff_validateRect(obj, pos, &pos, srcRect, &srcRect)){
                                    printf("ERROR, StreamContext, bitblit failed.\n");
                                } else if(srcRect.width > 0 && srcRect.height > 0){
                                    STFramebuffDrawRect* rect = &rects[rectsUse];
                                    //
                                    //printf("Stream-rect-added-black row(%d) pos(%d, %d) srcRect(%d, %d)-(+%d, +%d).\n", iRow, pos.x, pos.y, srcRect.x, srcRect.y, srcRect.width, srcRect.height);
                                    //
                                    rect->iRow      = rowsPassedCount;
                                    rect->plane     = NULL; //NULL means: 'use the blackLine array'
                                    rect->posCur    = pos;
                                    rect->srcRectX  = srcRect.x;
                                    rect->srcRectWidth = srcRect.width;
                                    rect->srcRectY  = srcRect.y;
                                    rect->srcRectYAfterEnd = srcRect.y + srcRect.height;
                                    //
                                    rectAdded = 1;
                                    rectsUse++;
                                    linesSz += srcRect.height;
                                } else {
                                    //printf("Stream-rect-ignored-black row(%d) pos(%d, %d) srcRect(%d, %d)-(+%d, +%d).\n", iRow, pos.x, pos.y, srcRect.x, srcRect.y, srcRect.width, srcRect.height);
                                }
                            }
                        }
                        //next row
                        rowsPassedCount++;
                        yTop += row->height;
                        if(yTop > obj->height || rectsUse >= rectsSz){
                            //stop
                            break;
                        }
                    }
                    //
                    if(yTopBefore == yTop /*avoid-infinite-cycle*/ || yTop > obj->height || rectsUse >= rectsSz){
                        //stop
                        break;
                    }
                }
            }
            //prepare lines array
            if(linesSz > 0){
                int linesUse = 0;
                STFramebuffDrawLine* lines = (STFramebuffDrawLine*)malloc(sizeof(STFramebuffDrawLine) * linesSz);
                //printf("Drawing planned: %d lines, %d rowsPassedCount.\n", linesSz, rowsPassedCount);
                if(lines != NULL){
                    memset(lines, 0, sizeof(STFramebuffDrawLine) * linesSz);
                    //build draw plan (lines per row)
                    int i, iRow = -1, rowStartRectIdx = 0, rowRectsCount = 0;
                    for(i = 0; i < rectsUse; i++){
                        STFramebuffDrawRect* rect = &rects[i];
                        if(iRow != rect->iRow){
                            if(rowRectsCount > 0){
                                Framebuff_drawToPtrPlannedBuildRowPlan_(obj, plyr, dst, &rects[rowStartRectIdx], rowRectsCount, lines, &linesUse);
                            }
                            iRow = rect->iRow;
                            rowStartRectIdx = i;
                            rowRectsCount = 0;
                        }
                        //add to row
                        rowRectsCount++;
                    }
                    //flush last row
                    if(rowRectsCount > 0){
                        Framebuff_drawToPtrPlannedBuildRowPlan_(obj, plyr, dst, &rects[rowStartRectIdx], rowRectsCount, lines, &linesUse);
                    }
                    //
                    //printf("Framebuff_drawToPtrPlanned, %d rects, %d lines.\n", rectsUse, linesUse);
                    //Execute tasks
                    {
                        STFramebuffDrawTaskPlaned* tt = malloc(sizeof(STFramebuffDrawTaskPlaned) * (plyr->threads.use + 1));
                        if(tt != NULL){
                            memset(tt, 0, sizeof(STFramebuffDrawTaskPlaned) * (plyr->threads.use + 1));
                            //
                            int iLineStart = 0, ttUse = 0, linesPerThread = (linesUse / (plyr->threads.use + 1));
                            int i; for(i = 0; i < plyr->threads.use; i++){
                                STThread* thread = &plyr->threads.arr[i];
                                STFramebuffDrawTaskPlaned* task = &tt[ttUse];
                                //
                                task->dst   = dst;
                                task->lines = &lines[iLineStart];
                                task->linesSz = linesPerThread;
                                if((iLineStart + task->linesSz) > linesUse){
                                    task->linesSz = linesUse - iLineStart;
                                }
                                //
                                pthread_mutex_lock(&dst->mutex);
                                {
                                    dst->tasksPendCount++;
                                }
                                pthread_mutex_unlock(&dst->mutex);
                                if(0 != Thread_addTask(thread, Framebuff_drawPplanedTaskFunc_, task)){
                                    //printf("ERROR, Thread_addTask failed.\n"); //ToDo: comment this print (risk to noisy).
                                    pthread_mutex_lock(&dst->mutex);
                                    {
                                        dst->tasksPendCount--;
                                    }
                                    pthread_mutex_unlock(&dst->mutex);
                                } else {
                                    //next
                                    iLineStart += task->linesSz;
                                    ttUse++;
                                }
                            }
                            //task to draw remaining lines on this same thread
                            {
                                STFramebuffDrawTaskPlaned* task = &tt[ttUse];
                                task->dst   = dst;
                                task->lines = &lines[iLineStart];
                                task->linesSz = linesUse - iLineStart;
                                ttUse++;
                                pthread_mutex_lock(&dst->mutex);
                                {
                                    dst->tasksPendCount++;
                                }
                                pthread_mutex_unlock(&dst->mutex);
                                Framebuff_drawPplanedTaskFunc_(task);
                            }
                            //wait for tasks
                            {
                                pthread_mutex_lock(&dst->mutex);
                                while(dst->tasksPendCount > 0){
                                    K_ASSERT(dst->tasksPendCount >= 0)
                                    pthread_cond_wait(&dst->cond, &dst->mutex);
                                }
                                pthread_mutex_unlock(&dst->mutex);
                            }
                            free(tt);
                            tt = NULL;
                        }
                    }
                    //
                    free(lines);
                    lines = NULL;
                }
            }
            if(rects != NULL){
                free(rects);
                rects = NULL;
            }
        }
        //
        obj->screen.isSynced = 0;
    }
    return r;
}


int Framebuff_drawToPtr(STFramebuff* obj, struct STPlayer_* plyr, STFramebuffPtr* dst){
    //return Framebuff_drawToPtrUnplanned(obj, plyr, dst);
    return Framebuff_drawToPtrPlanned(obj, plyr, dst);
}

//STThread

void Thread_init(STThread* obj){
    memset(obj, 0, sizeof(*obj));
    //
    pthread_mutex_init(&obj->mutex, NULL);
    pthread_cond_init(&obj->cond, NULL);
}

void Thread_release(STThread* obj){
    pthread_mutex_lock(&obj->mutex);
    {
        while(obj->isRunning && obj->tasks.use > 0){
            obj->stopFlag = 1;
            pthread_cond_broadcast(&obj->cond);
            pthread_cond_wait(&obj->cond, &obj->mutex);
        }
        //tasks
        if(obj->tasks.arr != NULL){
            int i; for(i = 0; i < obj->tasks.use; i++){
                STThreadTask* t = obj->tasks.arr[i];
                free(t);
            }
            free(obj->tasks.arr);
            obj->tasks.arr = NULL;
        }
        obj->tasks.use = 0;
        obj->tasks.sz = 0;
    }
    pthread_mutex_unlock(&obj->mutex);
    pthread_cond_destroy(&obj->cond);
    pthread_mutex_destroy(&obj->mutex);
}

void* Thread_runMethod_(void* param){
    STThread* obj = (STThread*)param;
    pthread_mutex_lock(&obj->mutex);
    //flag as started
    {
        obj->isRunning = 1;
        pthread_cond_broadcast(&obj->cond);
        printf("Thread, run-method started.\n");
    }
    //cycle
    while(!obj->stopFlag || obj->tasks.use > 0){
        STThreadTask* t = NULL;
        //search for a task
        if(obj->tasks.use > 0){
            t = obj->tasks.arr[0];
            obj->tasks.use--;
            pthread_cond_broadcast(&obj->cond);
            //fill gap
            {
                int i; for(i = 0; i < obj->tasks.use; i++){
                    obj->tasks.arr[i] = obj->tasks.arr[i + 1];
                }
            }
        }
        //action
        if(t == NULL){
            //wait for a task or flag
            pthread_cond_wait(&obj->cond, &obj->mutex);
        } else {
            //run task (unclocked)
            pthread_mutex_unlock(&obj->mutex);
            {
                if(t->func != NULL){
                    (*t->func)(t->param);
                }
                free(t);
                t = NULL;
            }
            pthread_mutex_lock(&obj->mutex);
        }
    }
    //flag as ended
    {
        obj->isRunning = 0;
        pthread_cond_broadcast(&obj->cond);
        printf("Thread, run-method ended.\n");
    }
    pthread_mutex_unlock(&obj->mutex);
    return 0;
}

    
int Thread_start(STThread* obj){
    int r = -1;
    pthread_mutex_lock(&obj->mutex);
    if(!obj->isRunning){
        int isJoinable = 0;
        pthread_attr_t attrs;
        pthread_attr_init(&attrs);
        //Set joinabled or detached (determine when system's resources will be released)
        if(pthread_attr_setdetachstate(&attrs, (isJoinable ? PTHREAD_CREATE_JOINABLE : PTHREAD_CREATE_DETACHED)) != 0){
            printf("Error calling 'pthread_attr_setdetachstate'.\n");
            exit(1);
        }
        {
            obj->stopFlag = 0;
            obj->isRunning = 1;
            if(0 != pthread_create(&obj->thread, &attrs, Thread_runMethod_, obj)){
                obj->isRunning = 0;
                printf("Error calling 'pthread_create'.\n");
            } else {
                printf("Thread, started.\n");
                r = 0;
            }
        }
        pthread_attr_destroy(&attrs);
    }
    pthread_mutex_unlock(&obj->mutex);
    return r;
}

int Thread_stopFlag(STThread* obj){
    pthread_mutex_lock(&obj->mutex);
    {
        obj->stopFlag = 1;
        pthread_cond_broadcast(&obj->cond);
    }
    pthread_mutex_unlock(&obj->mutex);
    return 0;
}

int Thread_waitForAll(STThread* obj){
    pthread_mutex_lock(&obj->mutex);
    {
        while(obj->isRunning || obj->tasks.use > 0){
            pthread_cond_wait(&obj->cond, &obj->mutex);
        }
    }
    pthread_mutex_unlock(&obj->mutex);
    return 0;
}

int Thread_addTask(STThread* obj, ThreadTaskFunc func, void* param){
    int r = -1;
    pthread_mutex_lock(&obj->mutex);
    {
        //resize
        while((obj->tasks.use + 1) > obj->tasks.sz){
            STThreadTask** arrN = malloc(sizeof(STThreadTask*) * (obj->tasks.use + 1));
            if(arrN == NULL){
                break;
            } else {
                if(obj->tasks.arr != NULL){
                    if(obj->tasks.use > 0){
                        memcpy(arrN, obj->tasks.arr, sizeof(STThreadTask*) * obj->tasks.use);
                    }
                    free(obj->tasks.arr);
                    obj->tasks.arr = NULL;
                }
                obj->tasks.arr = arrN;
                obj->tasks.sz = obj->tasks.use + 1;
            }
        }
        //add
        if((obj->tasks.use + 1) <= obj->tasks.sz){
            STThreadTask* t = malloc(sizeof(STThreadTask));
            if(t != NULL){
                memset(t, 0, sizeof(*t));
                t->func = func;
                t->param = param;
                obj->tasks.arr[obj->tasks.use] = t;
                obj->tasks.use++;
                pthread_cond_broadcast(&obj->cond);
                //
                r = 0;
            }
        }
    }
    pthread_mutex_unlock(&obj->mutex);
    return r;
}

//STPrintedDef
//Used to avoid printing the same info multiple times

void PrintedInfo_init(STPrintedInfo* obj){
    memset(obj, 0, sizeof(*obj));
}

void PrintedInfo_release(STPrintedInfo* obj){
    if(obj->device != NULL){
        free(obj->device);
        obj->device = NULL;
    }
}

//

int PrintedInfo_set(STPrintedInfo* obj, const char* device, const int srcFmt, const int dstFmt){ //applies info
    if(device != obj->device){
        if(obj->device != NULL){ free(obj->device); obj->device = NULL; }
        if(device != NULL){
            int deviceLen = strlen(device);
            obj->device = malloc(deviceLen + 1);
            memcpy(obj->device, device, deviceLen + 1);
        }
    }
    obj->srcFmt = srcFmt;
    obj->dstFmt = dstFmt;
    return 0;
}

int PrintedInfo_touch(STPrintedInfo* obj){  //updated 'last' time
    //printf("Touching: '%s' / %d / %d.\n", obj->device, obj->srcFmt, obj->dstFmt);
    gettimeofday(&obj->last, NULL);
    return 0;
}
