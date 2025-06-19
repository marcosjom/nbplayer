//
//  nbplayer.c
//
//
//  Created by Marcos Ortega on 11/3/24.
//

#define K_DEBUG             //if defined, internal debug code is enabled
//#define K_USE_NATIVE_PRINTF //if defined, K_LOG method is mapped to 'printf()'; this allows compilation time warnings for printf-string-formats.
#define K_USE_MPLANE        //if defined, _MPLANE buffers are used instead of single-plane (NOTE: '_MPLANE' seems not to work with G_CROP ctl)

#define _GNU_SOURCE         //for <netdb.h>

//

#include <stdio.h>          //for printf, generics
#include <stdlib.h>         //for "abort", "srand()", "rand()"
#include <stdint.h>         //for uint32_t
#include <stdarg.h>         //for va_list
#include <linux/version.h>  //for KERNEL_VERSION() macro
#include <linux/videodev2.h>
#include <sys/ioctl.h>      //for ioctl
#include <sys/mman.h>       //for mmap
#include <string.h>         //for memset
#include <fcntl.h>          //for open
#include <unistd.h>         //for open/read/write
#include <errno.h>          //for EAGAIN
#include <pthread.h>        //thread, mutex, cond
#include <unistd.h>         //for usleep()
#include <linux/fb.h>       //for framebuffer (/dev/fb...)
#include <libv4l2.h>        //for v4l2_* calls, gives support to v4l1 drivers and extra pixel formats, https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/libv4l-introduction.html
                            //requires '-lv4l2' at compilation.
//socket
#include <unistd.h>         //for close() de sockets
#include <netdb.h>          //for 'getaddrinfo_a()' asynchronus hostname resolution
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
#   define  IF_DEBUG(V)         V
#   define  K_ASSERT(V)         if(!(V)){ K_LOG_CRITICAL("Assert failed line %d, func '%s': '%s'\n", __LINE__, __func__, #V); assert(0); exit(-1); }
#   define  K_ASSERT_NATIVE(V)  if(!(V)){ printf("Assert failed line %d, func '%s': '%s'\n", __LINE__, __func__, #V); assert(0); exit(-1); }
#else
#   define  IF_DEBUG(V)     //empty
#   define  K_ASSERT(V)     //empty
#   define  K_ASSERT_NATIVE(V) //empty
#endif

//default values

#define K_DEF_REPRINTS_HIDE_SECS    (60 * 60) //redundant info printing skip time (like devices properties, once printed, wait this ammount of secs before printing them again).
#define K_DEF_THREADS_EXTRA_AMM     0       //ammount of extra threads (for rendering). Note: best efficiency is '0 extra threads' (single thread), best performance is '1 extra thread' dual-threads.
#define K_DEF_CONN_TIMEOUT_SECS     60      //seconds to wait for connection-inactivity-timeout.
#define K_DEF_CONN_RETRY_WAIT_SECS  5       //seconds to wait before trying to connect again.
#define K_DEF_DECODER_TIMEOUT_SECS  5       //seconds to wait for decoder-inactivity-timeout (frames are arriving from src, decoder is explicit-on but not producing output).
#define K_DEF_DECODER_RETRY_WAIT_SECS  5    //seconds to wait before trying to open device again.
#define K_DEF_DECODERS_MAX_AMM      16       //ammount of maximun simultaneous opened decoders.
#define K_DEF_DECODERS_PEEK_MAX_SECS 2      //seconds max to allow a decoder to be in peek state (peek should take milliseconds if IDR-frame is available)
#define K_DEF_ANIM_WAIT_SECS        10      //seconds to wait between streams position animations.
#define K_DEF_ANIM_PRE_RENDER_SECS  2       //seconds to start rendering offscree-stream before next streams position animations (to ensure something will be rendered before entering to screen).
#define K_DEF_FRAMES_PER_SEC        25      //fps / screen-refreshs-per-second. Note: frames decoding is done as fast as posible, screen-refreshs draws the latest decoded frames to the screen.
#define K_DEF_DRAW_MODE             ENPlayerDrawMode_Src

#ifndef SOCKET
#   define SOCKET           int
#endif

#ifndef INVALID_SOCKET
#   define INVALID_SOCKET   (SOCKET)(~0)
#endif

#define CALL_IOCTL(D, M) \
    D = M; \
    if(D != 0){ \
        const STErrCode* err = _getErrCode(errno); \
        if(err == NULL){ \
            K_LOG_ERROR("" #M " returned errno(%d) at line %d.\n", errno, __LINE__); \
        } else { \
            K_LOG_ERROR("" #M " returned '%s' at line %d.\n", err->str, __LINE__); \
        } \
    } else { \
        K_LOG_VERBOSE("" #M " success.\n"); \
    } \

//Log

typedef enum ENLogLevel_ {
    ENLogLevel_Critical = 0,
    ENLogLevel_Error,
    ENLogLevel_Warning,
    ENLogLevel_Info,
    ENLogLevel_Verbose,
    //
    ENLogLevel_Count
} ENLogLevel;

//log-core
static pthread_mutex_t _logMmutex;
static int _logBuffTmpSz            = 0;
static char* _logBuffTmp            = NULL;
static ENLogLevel _logLvlMax        = ENLogLevel_Info;

//log-to-circular-file
static FILE* _logStream             = NULL;
static unsigned long _logStreamPos  = 0;
static unsigned long _logStreamMaxSz = 0;   //if non-zero, the file will return to position zero once this ammount of bytes havebeen reached.

//log-config
static int _logStdOutOff = 0;
static int _logStdErrOff = 0;

//
void __logInit(void);
void __logEnd(void);

//
int __logOpenFile(const char* path);
void __log(const ENLogLevel lvl, const char* fmt, ...);

#ifdef K_USE_NATIVE_PRINTF
#   define K_LOG_CRITICAL(FMT, ...) if(ENLogLevel_Critical <= _logLvlMax ) printf("CRITICAL, " FMT, ##__VA_ARGS__)
#   define K_LOG_ERROR(FMT, ...)    if(ENLogLevel_Error <= _logLvlMax ) printf("ERROR, " FMT, ##__VA_ARGS__)
#   define K_LOG_WARN(FMT, ...)     if(ENLogLevel_Warning <= _logLvlMax ) printf("WARN, " FMT, ##__VA_ARGS__)
#   define K_LOG_INFO(FMT, ...)     if(ENLogLevel_Info <= _logLvlMax ) printf(FMT, ##__VA_ARGS__)
#   define K_LOG_VERBOSE(FMT, ...)  if(ENLogLevel_Verbose <= _logLvlMax ) printf(FMT, ##__VA_ARGS__)
#else
#   define K_LOG_CRITICAL(FMT, ...) __log(ENLogLevel_Critical, FMT, ##__VA_ARGS__)
#   define K_LOG_ERROR(FMT, ...)    __log(ENLogLevel_Error, FMT, ##__VA_ARGS__)
#   define K_LOG_WARN(FMT, ...)     __log(ENLogLevel_Warning, FMT, ##__VA_ARGS__)
#   define K_LOG_INFO(FMT, ...)     __log(ENLogLevel_Info, FMT, ##__VA_ARGS__)
#   define K_LOG_VERBOSE(FMT, ...)  __log(ENLogLevel_Verbose, FMT, ##__VA_ARGS__)
#endif

//v4l generic error codes

//https://www.kernel.org/doc/html/v4.8/media/uapi/gen-errors.html#id1

typedef struct STErrCode_ {
    int         value;
    const char* str;    //value in string form
    const char* desc;
} STErrCode;

const STErrCode _errCodes[] = {
    { EAGAIN, "EAGAIN", "The ioctl can’t be handled because the device is in state where it can’t perform it. This could happen for example in case where device is sleeping and ioctl is performed to query statistics. It is also returned when the ioctl would need to wait for an event, but the device was opened in non-blocking mode." }
    , { EWOULDBLOCK, "EWOULDBLOCK", "The ioctl can’t be handled because the device is in state where it can’t perform it. This could happen for example in case where device is sleeping and ioctl is performed to query statistics. It is also returned when the ioctl would need to wait for an event, but the device was opened in non-blocking mode." }
    , { EBADF, "EBADF", "The file descriptor is not a valid." }
    , { EBUSY, "EBUSY", "The ioctl can’t be handled because the device is busy. This is typically return while device is streaming, and an ioctl tried to change something that would affect the stream, or would require the usage of a hardware resource that was already allocated. The ioctl must not be retried without performing another action to fix the problem first (typically: stop the stream before retrying)." }
    , { EFAULT, "EFAULT", "There was a failure while copying data from/to userspace, probably caused by an invalid pointer reference." }
    , { EINVAL, "EINVAL", "One or more of the ioctl parameters are invalid or out of the allowed range. This is a widely used error code. See the individual ioctl requests for specific causes." }
    , { ENODEV, "ENODEV", "Device not found or was removed." }
    , { ENOMEM, "ENOMEM", "There’s not enough memory to handle the desired operation." }
    , { ENOTTY, "ENOTTY", "The ioctl is not supported by the driver, actually meaning that the required functionality is not available, or the file descriptor is not for a media device." }
    , { ENOSPC, "ENOSPC", "On USB devices, the stream ioctl’s can return this error, meaning that this request would overcommit the usb bandwidth reserved for periodic transfers (up to 80% of the USB bandwidth)." }
    , { EPERM, "EPERM", "Permission denied. Can be returned if the device needs write permission, or some special capabilities is needed (e. g. root)" }
};

const STErrCode* _getErrCode(const int value);

//Nal types (reminder):
/*
7.4.1.2.3 Order of NAL units and coded pictures and association to access units
 An access unit consists of one primary coded picture,
 zero or more corresponding redundant coded pictures, and zero or more non-VCL NAL units.

 The first access unit in the bitstream starts with the first NAL unit of the bitstream.

 The first of any of the following NAL units after the last VCL NAL unit of a primary coded picture specifies the start of a new access unit:
 – access unit delimiter NAL unit (when present),
 – sequence parameter set NAL unit (when present),
 – picture parameter set NAL unit (when present),
 – SEI NAL unit (when present),
 – NAL units with nal_unit_type in the range of 14 to 18, inclusive (when present),
 – first VCL NAL unit of a primary coded picture (always present).
 
 The constraints for the detection of the first VCL NAL unit of a primary coded picture are specified in clause 7.4.1.2.4.
 
 The following constraints shall be obeyed by the order of the coded pictures and non-VCL NAL units within an access unit:
 
 – When an access unit delimiter NAL unit is present, it shall be the first NAL unit. There shall be at most one access unit delimiter NAL unit in any access unit.
 – When any SEI NAL units are present, they shall precede the primary coded picture.
 – When an SEI NAL unit containing a buffering period SEI message is present, the buffering period SEI message shall be the first SEI message payload of the first SEI NAL unit in the access unit.
 – The primary coded picture shall precede the corresponding redundant coded pictures.
 – When redundant coded pictures are present, they shall be ordered in ascending order of the value of redundant_pic_cnt.
 – When a sequence parameter set extension NAL unit is present, it shall be the next NAL unit after a sequence parameter set NAL unit having the same value of seq_parameter_set_id as in the sequence parameter set extension NAL unit.
 – When one or more coded slice of an auxiliary coded picture without partitioning NAL units is present, they shall follow the primary coded picture and all redundant coded pictures (if any).
 – When an end of sequence NAL unit is present, it shall follow the primary coded picture and all redundant coded pictures (if any) and all coded slice of an auxiliary coded picture without partitioning NAL units (if any).
 – When an end of stream NAL unit is present, it shall be the last NAL unit.
 – NAL units having nal_unit_type equal to 0, 12, or in the range of 20 to 31, inclusive, shall not precede the first VCL NAL unit of the primary coded picture.
 70 Rec. ITU-T H.264 (06/2019)
 
 NOTE 2 – Sequence parameter set NAL units or picture parameter set NAL units may be present in an access unit, but cannot follow the last VCL NAL unit of the primary coded picture within the access unit, as this condition would specify the start of a new access unit.
 NOTE 3 – When a NAL unit having nal_unit_type equal to 7 or 8 is present in an access unit, it may or may not be referred to in the coded pictures of the access unit in which it is present, and may be referred to in coded pictures of subsequent access units.
 The structure of access units not containing any NAL units with nal_unit_type equal to 0, 7, 8, or in the range of 12 to 18, inclusive, or in the range of 20 to 31, inclusive, is shown in Figure 7-1.
*/

typedef enum ENNalTypeGrp_ {
    ENNalTypeGrp_NonVCL = 0,
    ENNalTypeGrp_VCL,
    ENNalTypeGrp_Stap_A,
    ENNalTypeGrp_Stap_B,
    ENNalTypeGrp_MTAP16,
    ENNalTypeGrp_MTAP24,
    ENNalTypeGrp_FU_A,
    ENNalTypeGrp_FU_B,
    //
    ENNalTypeGrp_Count
} ENNalTypeGrp;

typedef struct STNalTypeDesc_ {
    int             type;   //must match index
    ENNalTypeGrp    grp;
    const char*     desc;
} STNalTypeDesc;

const static STNalTypeDesc _naluDefs[] = {
    { 0, ENNalTypeGrp_NonVCL, "Unspecified" },
    { 1, ENNalTypeGrp_VCL, "Coded slice of a non-IDR picture slice_layer_without_partitioning_rbsp( )" },
    { 2, ENNalTypeGrp_VCL, "Coded slice data partition A slice_data_partition_a_layer_rbsp( )" },
    { 3, ENNalTypeGrp_VCL, "Coded slice data partition B slice_data_partition_b_layer_rbsp( )" },
    { 4, ENNalTypeGrp_VCL, "Coded slice data partition C slice_data_partition_c_layer_rbsp( )" },
    { 5, ENNalTypeGrp_VCL, "Coded slice of an IDR picture slice_layer_without_partitioning_rbsp( )" },
    { 6, ENNalTypeGrp_NonVCL, "Supplemental enhancement information (SEI) sei_rbsp( )" },
    { 7, ENNalTypeGrp_NonVCL, "Sequence parameter set seq_parameter_set_rbsp( )" },
    { 8, ENNalTypeGrp_NonVCL, "Picture parameter set pic_parameter_set_rbsp( )" },
    { 9, ENNalTypeGrp_NonVCL, "Access unit delimiter access_unit_delimiter_rbsp( )" },
    { 10, ENNalTypeGrp_NonVCL, "End of sequence end_of_seq_rbsp( )" },
    { 11, ENNalTypeGrp_NonVCL, "End of stream end_of_stream_rbsp( )" },
    { 12, ENNalTypeGrp_NonVCL, "Filler data filler_data_rbsp( )" },
    { 13, ENNalTypeGrp_NonVCL, "Sequence parameter set extension seq_parameter_set_extension_rbsp( )" },
    { 14, ENNalTypeGrp_NonVCL, "Prefix NAL unit prefix_nal_unit_rbsp( )" },
    { 15, ENNalTypeGrp_NonVCL, "Subset sequence parameter set subset_seq_parameter_set_rbsp( )" },
    { 16, ENNalTypeGrp_NonVCL, "Depth parameter set depth_parameter_set_rbsp( )" },
    { 17, ENNalTypeGrp_NonVCL, "Reserved" },
    { 18, ENNalTypeGrp_NonVCL, "Reserved" },
    { 19, ENNalTypeGrp_NonVCL, "Coded slice of an auxiliary coded picture without partitioning slice_layer_without_partitioning_rbsp( )" },
    { 20, ENNalTypeGrp_NonVCL, "Coded slice extension slice_layer_extension_rbsp( )" },
    { 21, ENNalTypeGrp_NonVCL, "Coded slice extension for depth view components slice_layer_extension_rbsp( )" },
    { 22, ENNalTypeGrp_NonVCL, "Reserved" },
    { 23, ENNalTypeGrp_NonVCL, "Reserved" },
    { 24, ENNalTypeGrp_Stap_A, "Single-time aggregation packet" },
    { 25, ENNalTypeGrp_Stap_B, "Single-time aggregation packet" },
    { 26, ENNalTypeGrp_MTAP16, "Multi-time aggregation packet" },
    { 27, ENNalTypeGrp_MTAP24, "Multi-time aggregation packet" },
    { 28, ENNalTypeGrp_FU_A, "Fragmentation unit" },
    { 29, ENNalTypeGrp_FU_B, "Fragmentation unit" },
    { 30, ENNalTypeGrp_NonVCL, "Unspecified" },
    { 31, ENNalTypeGrp_NonVCL, "Unspecified" },
};

//

struct STPlayer_;
struct STBuffer_;
struct STPlane_;
struct STFramebuffsGrp_;

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

//STFramebuffDrawRect

typedef struct STFramebuffDrawRect_ {
    int                     iRow;
    struct STFramebuff_*    fb;     //dst
    struct STPlane_*        plane;  //src-bitmap
    int                     srcRectX;       //
    int                     srcRectWidth;   //
    int                     srcRectY;       //will be updated
    int                     srcRectYAfterEnd;
    //
    STFbPos                 posCur;         //current dst position (it will be updated)
} STFramebuffDrawRect;

//STFramebuffDrawLine

typedef struct STFramebuffDrawLine_ {
    unsigned char*   dst;
    unsigned char*   src;
    int     sz;
} STFramebuffDrawLine;

//STFramebuffPtr

typedef struct STFramebuffPtr_ {
    unsigned char*  ptr;
    int             ptrSz;
    int             isSynced;
} STFramebuffPtr;

void FramebuffPtr_init(STFramebuffPtr* obj);
void FramebuffPtr_release(STFramebuffPtr* obj);

//STFramebuff

typedef struct STFramebuff_ {
    //cfg
    struct {
        char*           device; // "/dev/fb0"
    } cfg;
    STFramebuffPtr      offscreen;  //offscreen buffer (malloc)
    STFramebuffPtr      screen;     //screen buffer (mmap)
    void*               blackLine;  //data for a balack full-line (bytesPerLn)
    int                 blackLineSz; //bytesPerLn
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
int Framebuff_open(STFramebuff* obj, const char* device);
int Framebuff_validateRect(STFramebuff* obj, STFbPos pPos, STFbPos* dstPos, STFbRect pSrcRect, STFbRect* dstRect);
int Framebuff_bitblit(STFramebuff* obj, STFramebuffPtr* dst, STFbPos dstPos, const struct STPlane_* srcPixs, STFbRect srcRect);
int Framebuff_drawRowsBuildPlan(STFramebuff* obj, STFramebuffPtr* dst, STFramebuffDrawRect* rects, const int rectsUse, STFramebuffDrawLine* lines, int linesSz, int* linesUse);

//STFramebuffsGrpFb

typedef enum ENFramebuffsGrpFbLocation_ {
    ENFramebuffsGrpFbLocation_Free = 0, //fb will be added at user-specified (x, y) location
    ENFramebuffsGrpFbLocation_Right,    //fb will expand the grp to the right
    ENFramebuffsGrpFbLocation_Bottom,   //fb will expand the grp to the bottom
    ENFramebuffsGrpFbLocation_Left,     //fb will expand the grp to the left
    ENFramebuffsGrpFbLocation_Top,      //fb will expand the grp to the top
    //
    ENFramebuffsGrpFbLocation_Count
} ENFramebuffsGrpFbLocation;


typedef struct STFramebuffsGrpFb_ {
    STFramebuff*    fb;
    int             x; //relative to group
    int             y; //relative to group
    //cfg
    struct {
        ENFramebuffsGrpFbLocation location;
    } cfg;
} STFramebuffsGrpFb;

void FramebuffsGrpFb_init(STFramebuffsGrpFb* obj);
void FramebuffsGrpFb_release(STFramebuffsGrpFb* obj);

//STFramebuffsGrp
//fbs are grouped by 'pixFmt'

typedef struct STFramebuffsGrp_ {
    int             pixFmt;
    int             isClosed;       //do not add new famebuffers to this layout/group (activated by param '--frameBufferNewGrps')
    //cfg
    struct {
        int         animSecsWaits;
        int         animPreRenderSecs;
    } cfg;
    //limits
    int             xLeft;
    int             yTop;
    int             xRightNxt;
    int             yBottomNxt;
    //
    int             isSynced;
    //fbs
    struct {
        STFramebuffsGrpFb*   arr;
        int                  use;
        int                  sz;
    } fbs;
    //streams
    struct {
        struct STStreamContext_** arr;
        int                  use;
        int                  sz;
    } streams;
    //layout
    struct {
        //layout total size
        int             width;   //rigth side of all elements
        int             height;  //lower size of all elements
        //rows
        struct {
            STFbLayoutRow*  arr;
            int             use;
            int             sz;
            int             rectsCount;
        } rows;
        //anim
        struct {
            unsigned long msWait;   //currently waiting
            int         iRowFirst;  //current top row
            int         yOffset;    //current offset
        } anim;
    } layout;
} STFramebuffsGrp;

void FramebuffsGrp_init(STFramebuffsGrp* obj);
void FramebuffsGrp_release(STFramebuffsGrp* obj);
//
int FramebuffsGrp_addFb(STFramebuffsGrp* obj, STFramebuff* fb, const ENFramebuffsGrpFbLocation location, const int x, const int y);
int FramebuffsGrp_addStream(STFramebuffsGrp* obj, struct STStreamContext_* ctx);
//
int FramebuffsGrp_layoutStart(STFramebuffsGrp* obj);    //resets 'isFound'
int FramebuffsGrp_layoutEnd(STFramebuffsGrp* obj);      //removes records without 'isFound' flag, and organizes in order
int FramebuffsGrp_layoutAdd(STFramebuffsGrp* obj, int streamId, const STFbSize size); //updates or add the stream, flags it as 'isFound'
//
int FramebuffsGrp_layoutFindStreamId(STFramebuffsGrp* obj, int streamId);    //find this stream current location in scene
int FramebuffsGrp_layoutAnimTick(STFramebuffsGrp* obj, const int ms, struct STPlayer_* plyr, const int msMinToDoDrawPlanNextPos, const int doDrawPlanIfAnimating);
//
int FramebuffsGrp_drawGetRects(STFramebuffsGrp* obj, struct STPlayer_* plyr, const int yOffset, STFramebuffDrawRect* rects, int rectsSz, int* dstRectsUse);

//STPlane

typedef struct STPlane_ {
    int             isOrphanable;   //obtained V4L2_BUF_CAP_SUPPORTS_ORPHANED_BUFS flag when requeting buffers, must be unmaped and closed to be released.
    int             isMmaped;   //0, the plane ptr was mallocated, 1, the plane ptr is owned externally and should not be freed
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
    int         isOrphanable;   //obtained V4L2_BUF_CAP_SUPPORTS_ORPHANED_BUFS flag when requeting buffers, must be unmaped and closed to be released.
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
    char*       name;           //internal value, for dbg
    int         type;           //V4L2_BUF_TYPE_VIDEO_OUTPUT(_MPLANE), V4L2_BUF_TYPE_VIDEO_CAPTURE(_MPLANE)
    STBuffer*   arr;
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
int Buffers_setFmt(STBuffers* obj, int fd, int fmt, int planesPerBuffer, int sizePerPlane, const int getCompositionRect, const int print);
int Buffers_getCompositionRect(STBuffers* obj, int fd, STFbRect* dstRect);
int Buffers_allocBuffs(STBuffers* obj, int fd, int ammount, const int print); //to dealloc buffers call this method with zero-ammount.
int Buffers_export(STBuffers* obj, int fd);
int Buffers_mmap(STBuffers* obj, int fd);
int Buffers_getUnqueued(STBuffers* obj, STBuffer** dstBuff, STBuffer* ignoreThis);        //get a buffer not enqueued yet
int Buffers_enqueueMinimun(STBuffers* obj, int fd, const int minimun);
int Buffers_enqueue(STBuffers* obj, int fd, STBuffer* srcBuff, const struct timeval* srcTimestamp);     //add to queue
int Buffers_dequeue(STBuffers* obj, int fd, STBuffer** dstBuff, struct timeval* dstTimestamp);     //remove from queue
int Buffers_start(STBuffers* obj, int fd);
int Buffers_stop(STBuffers* obj, int fd);
int Buffers_keepLastAsClone(STBuffers* obj, STBuffer* src);

//ENPlayerPollFdType

typedef enum ENPlayerPollFdType_ {
    ENPlayerPollFdType_Decoder = 0, //dec (decoder).fd
    ENPlayerPollFdType_SrcFile,     //file.fd
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
int VideoFrameStates_empty(STVideoFrameStates* obj);

//STVideoFrame
//In H264, an Access unit allways produces an output frame.
//IDR = Instantaneous Decoding Refresh

typedef struct STVideoFrame_ {
    STVideoFrameState   state;
    //accessUnit
    struct {
        int             isInvalid;              //payload must be discarded
        int             lastCompletedNalType;   //
        int             nalsCountPerType[32];   //counts of NALs contained by this frame, by type (32 max)
        //delimeter
        struct {
            int         isPresent;
            int         primary_pic_type;   //u(3)
            int         slicesAllowedPrimaryPicturePerType[32];   //slices allowed in a primary picture
        } delimeter;
    } accessUnit;
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
//
int VideoFrame_getNalsCount(const STVideoFrame* obj);
int VideoFrame_getNalsCountOfGrp(const STVideoFrame* obj, const ENNalTypeGrp grp);
int VideoFrame_setAccessUnitDelimiterFound(STVideoFrame* obj, const int primary_pic_type);

//STVideoFrames

typedef struct STVideoFrames_ {
    unsigned long   iSeqPushNext;   //iSeq to set to pushed frames
    unsigned long   iSeqPullNext;   //iSeq to set to pulled frames
    STVideoFrame**  arr;    //array of ptrs
    int             sz;
    int             use;
} STVideoFrames;

void VideoFrames_init(STVideoFrames* obj);
void VideoFrames_release(STVideoFrames* obj);
//
int VideoFrames_pullFrameForFill(STVideoFrames* obj, STVideoFrame** dst); //get from the right, reuse or creates a new one
int VideoFrames_getFramesForReadCount(STVideoFrames* obj); //peek from the left
int VideoFrames_pullFrameForRead(STVideoFrames* obj, STVideoFrame** dst); //get from the left
int VideoFrames_pushFrameOwning(STVideoFrames* obj, STVideoFrame* src); //add for future pull (reuse)

//STStreamContext

#define STREAM_CONTEXT_DECODER_SHOULD_BE_OPEN(OBJ)  ( \
                                                        !((OBJ)->shuttingDown.isActive && (OBJ)->shuttingDown.isPermanent) /*decoder is not permanently shutting/ed down*/ \
                                                        && ( \
                                                            (OBJ)->drawPlan.peekRemainMs > 0 /*image-props are required*/ \
                                                            || (OBJ)->drawPlan.hitsCount > 0 /*image will be at screen*/ \
                                                            ) \
                                                      ? 1 : 0 )

typedef struct STStreamContext_ {
    int                 streamId;   //defined by the player
    //cfg
    struct {
        char*           device; //"/dev/video10"
        char*           server; //ip or dns
        unsigned int    port;   //port
        int             keepAlive;   //network connection is kept alive when decoder is disabled.
        char*           path;   // "/folder/file.264"
        int             srcPixFmt; //V4L2_PIX_FMT_H264
        int             buffersAmmount;
        int             planesPerBuffer;
        int             sizePerPlane;
        int             dstPixFmt;
        int             connTimeoutSecs;
        int             decoderTimeoutSecs;
        int             animSecsWaits;
        int             animPreRenderSecs;
        unsigned long   framesSkip;     //frames to skip since the estart
        unsigned long   framesFeedMax;  //frames to feed and the stop feeding
    } cfg;
    //dec (decoder)
    struct {
        int             fd;
        int             shouldBeOpen;           //this decoder is allowed to be open (will be required and is under the limit of decorders count)
        unsigned long   msOpen;                 //time since open() returned this fd.
        unsigned long   msFirstFrameFed;        //time since open() that first frame was fed to decoder
        unsigned long   msFirstFrameOut;        //time since open() that first frame was produced to decoder
        unsigned long   framesInSinceOpen;      //ammount of frames fed to decoder since opened
        unsigned long   framesOutSinceOpen;     //ammount of frames produced since opened
        unsigned long   msWithoutFeedFrame;     //to detect decoder-timeout
        unsigned long   msToReopen;  //
        int             isWaitingForIDRFrame;
        STBuffers       src;
        STBuffers       dst;
        //frames
        struct {
            unsigned long       foundCount; //frames found available to fed
            unsigned long       fedCount;   //frames actually fed (= foundCount - cfg.framesSkip)
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
    //buff (read from file or socket)
    struct {
        unsigned char* buff;
        int         buffCsmd;   //left side consumed
        int         buffUse;    //right side producer
        int         buffSz;
        unsigned long screenRefreshSeqBlocking; //waiting for this screen refresh to be removed before continuing.
        //nal
        struct {
            int     zeroesSeqAccum;  //reading posible headers '0x00 0x00 0x00 0x01' (start of a NAL)
            int     startsCount;     //total NALs found (including the curently incomplete one)
        } nal;
    } buff;
    //file
    struct {
        int             fd;
        unsigned long   msWithoutRead;  //to detect connection-timeout
        unsigned long   msToReconnect;  //
    } file;
    //net
    struct {
        SOCKET          socket;         //async
        struct gaicb*   hostResolver;   //async
        unsigned long   msWithoutSend;  //to detect connection-timeout
        unsigned long   msWithoutRecv;  //to detect connection-timeout
        unsigned long   msToReconnect;  //
        //
        unsigned long   msSinceStart;   //connection start (resolve or socket)
        unsigned long   msToResolve;    //time since start to resolve host
        unsigned long   msToConnect;    //time since start to send request
        unsigned long   msToRespStart;  //time since start to receive response header
        unsigned long   msToRespHead;   //time since complete to receive response header
        unsigned long   msToRespBody;   //time since start to receive response body
        unsigned long   msToFirstUnit;  //time since complete to receive first unit
        //
        unsigned long   bytesSent;      //
        unsigned long   bytesRcvd;      //
        unsigned long   unitsRcvd;      //
        //req
        struct {
            char*       pay;
            int         payCsmd;        //sent
            int         payUse;
            int         paySz;
        } req;
        //resp
        struct {
            int         headerEndSeq;    //0 = [], 1 = ['\r'], 2 = ['\r', '\n'], 3 = ['\r', '\n', '\r']
            int         headerSz;        //
            int         headerEnded;     //"\r\n\r\n" found after connection
        } resp;
    } net;
    //drawPlan
    struct {
        //persistent between decoder closing/open
        int lastPixelformat; //last dec.dst.pixelformat value known
        STFbRect lastCompRect; //last dec.dst.composition with non-zero-sizes known
        int lastHeight;     //last dec.dst.composition.height non-zero value known
        //
        int peekRemainMs;   //this stream is allowed to decode to peek the image-size or stream-props
        int hitsCount;      //times this context will be used on the current draw-plan
    } drawPlan;
    //flushing
    struct {
        int     isActive;       //in progress
        int     isSrcDone;      //src flushed
        int     isCompleted;    //src and dst flushed
        int     msAccum;        //ms since started flushing
    } flushing;
    //flushing
    struct {
        int     isActive;       //in progress
        int     isCompleted;    //done
        int     isPermanent;    //will never be oepened again
        int     msAccum;        //ms since started shutingdown
    } shuttingDown;
} STStreamContext;

void StreamContext_init(STStreamContext* ctx);
void StreamContext_release(STStreamContext* ctx);

//
int StreamContext_isSame(STStreamContext* ctx, const char* device, const char* server, const unsigned int port, const char* resPath, int srcPixFmt /*V4L2_PIX_FMT_H264*/, int dstPixFmt /*V4L2_PIX_FMT_RGB565*/);
int StreamContext_open(STStreamContext* ctx, struct STPlayer_* plyr, const char* device, const char* server, const unsigned int port, const int keepAlive, const char* resPath, int srcPixFmt /*V4L2_PIX_FMT_H264*/, int buffersAmmount, int planesPerBuffer, int sizePerPlane, int dstPixFmt /*V4L2_PIX_FMT_RGB565*/, const int connTimeoutSecs, const int decoderTimeoutSecs, const unsigned long framesSkip, const unsigned long framesFeedMax);
int StreamContext_close(STStreamContext* ctx, struct STPlayer_* plyr);
//
int StreamContext_concatHttpRequest(STStreamContext* ctx, char* dst, int dstSz);

void StreamContext_updatePollMask_(STStreamContext* ctx, struct STPlayer_* plyr);
void StreamContext_updatePollMaskFile_(STStreamContext* ctx, struct STPlayer_* plyr);
//
int StreamContext_getMinBuffersForDst(STStreamContext* ctx, int* dstValue);
//
int StreamContext_initAndPrepareSrc(STStreamContext* ctx, int fd, const int buffersAmmount, const int print);
int StreamContext_initAndStartDst(STStreamContext* ctx, struct STPlayer_* plyr);
int StreamContext_stopAndCleanupBuffs(STStreamContext* ctx, STBuffers* buffs, int fd);
//
int StreamContext_eventsSubscribe(STStreamContext* ctx, int fd);
int StreamContext_eventsUnsubscribe(STStreamContext* ctx, int fd);

void StreamContext_tick(STStreamContext* ctx, struct STPlayer_* plyr, unsigned int ms);
int StreamContext_getPollEventsMask(STStreamContext* ctx);
int StreamContext_getPollEventsMaskFile(STStreamContext* ctx, struct STPlayer_* plyr);
void StreamContext_pollCallback(void* userParam, struct STPlayer_* plyr, const ENPlayerPollFdType type, int revents);

int StreamContext_flushStart(STStreamContext* ctx);
int StreamContext_flushTick(STStreamContext* ctx, const int ms, const char* srcLocation);

int StreamContext_shutdownStartByFileClosed_(STStreamContext* ctx, struct STPlayer_* plyr, const char* reason);
int StreamContext_shutdownStart(STStreamContext* ctx, struct STPlayer_* plyr, const int isPermanent);
int StreamContext_shutdownTick(STStreamContext* ctx, struct STPlayer_* plyr, const int ms, const char* srcLocation);

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

//STFramebuffsGrpFb

typedef enum ENPlayerDrawMode_ {
    ENPlayerDrawMode_Dst = 0,   //drawing order is relative to dst-pixels (ordered lines)
    ENPlayerDrawMode_Src,       //drawing order is relative to src-pixels
    //
    ENPlayerDrawMode_Count
} ENPlayerDrawMode;

typedef struct STPlayer_ {
    int                 streamIdNext;   //to assign to streams
    unsigned long long  msRunning;
    //cfg
    struct {
        int             extraThreadsAmm;
        int             connTimeoutSecs;
        int             connWaitReconnSecs;
        int             decoderTimeoutSecs;
        int             decoderWaitRecopenSecs;
        int             decodersMax;
        int             decodersToPeekSecs;
        int             animSecsWaits;
        int             animPreRenderSecs;
        int             screenRefreshPerSec;
        ENPlayerDrawMode drawMode;
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
        //grps (per pixFmt)
        struct {
            STFramebuffsGrp* arr;
            int             use;
            int             sz;
        } grps;
    } fbs;
    //streams
    struct {
        STStreamContext** arr;
        int             arrUse;
        int             arrSz;
        int             permShuttedDownCount; //amount of permanetly shutted down streams
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
            //src
            struct {
                //nals
                struct {
                    unsigned long long  started;    //header found { 0x00, 0x00, 0x00, 0x01 }
                    unsigned long long  completed;  //end-of-nal found (usually the next header)
                } nals;
                //frames (access units)
                struct {
                    unsigned long long  ignored;    //completed NALs ignored (not queued)
                    unsigned long long  queued;     //completed NALs queued
                    unsigned long long  queuedIDR;  //completed NALs queued are IDRs
                } frames;
            } src;
            //decoder
            struct {
                //fed (input)
                struct {
                    unsigned long long count;   //frames
                } fed;
                //got (output)
                struct {
                    unsigned long long msMin;   //ms-min
                    unsigned long long msMax;   //ms-max
                    unsigned long long msSum;   //ms-sum
                    unsigned long long count;   //frames
                    unsigned long long skipped;   //frames
                } got;
            } dec;
            //draw
            struct {
                unsigned long long msMin;   //ms-min
                unsigned long long msMax;   //ms-max
                unsigned long long msSum;   //ms-sum
                unsigned long long count;   //times
            } draw;
        } curSec;
    } stats;
    //anim
    struct {
        unsigned long   tickSeq;      //ticks
    } anim;
    //draw
    struct {
        pthread_mutex_t mutex;
        pthread_cond_t  cond;
        int             tasksPendCount; //tasks delegated to worker threads, waiting for results
    } draw;
    //peek
    struct {
        int     iNextStreamEval;        //to ensure peeking all-streams before repeating peeking
    } peek;
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
int Player_fbAdd(STPlayer* obj, const char* device, const ENFramebuffsGrpFbLocation location, const int locX, const int locY, const int animSecsWaits);
int Player_fbRemove(STPlayer* obj, STFramebuff* stream);
int Player_fbsCloseCurrentGrps(STPlayer* obj);

//streams
int Player_streamAdd(STPlayer* obj, const char* device, const char* server, const unsigned int port, const int keepAlive, const char* resPath, const int connTimeoutSecs, const int decoderTimeoutSecs, const unsigned long framesSkip, const unsigned long framesFeedMax);
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
    K_LOG_INFO("Params:\n");
    K_LOG_INFO("\n");
    K_LOG_INFO("-h, --help                prints this text.\n");
    K_LOG_INFO("-dcb, --disableCursorBlinking, writes '0' at '/sys/class/graphics/fbcon/cursor_blink'.\n");
    K_LOG_INFO("-t, --extraThreads num    extra threads for rendering (default: %d).\n", K_DEF_THREADS_EXTRA_AMM);
    K_LOG_INFO("-cto, --connTimeout num   seconds without conn activity to restart connection (default: %ds).\n", K_DEF_CONN_TIMEOUT_SECS);
    K_LOG_INFO("-crc, --connWaitReconnect num, seconds to wait before reconnect (default: %ds).\n", K_DEF_CONN_RETRY_WAIT_SECS);
    K_LOG_INFO("-dto, --decTimeout num    seconds without decoder output to restart decoder (default: %ds).\n", K_DEF_DECODER_TIMEOUT_SECS);
    K_LOG_INFO("-dro, --decWaitReopen num, seconds to wait before reopen decoder device (default: %ds).\n", K_DEF_DECODER_RETRY_WAIT_SECS);
    K_LOG_INFO("-aw, --animWait num       seconds between animation steps (default: %ds).\n", K_DEF_ANIM_WAIT_SECS);
    K_LOG_INFO("-fps, --framesPerSec num  screen frames/refresh per second (default: %d).\n", K_DEF_FRAMES_PER_SEC);
    K_LOG_INFO("-dm, --drawMode v         defines the drawing order:\n");
    K_LOG_INFO("                          dst; drawing lines will be arranged in dst-buffer order.\n");
    K_LOG_INFO("                          src; drawing lines will be arranged in src-buffer order.\n");
    K_LOG_INFO("\n");
    K_LOG_INFO("-fbl, --frameBufferLoc v  sets the layout location for the next framebuffers:\n");
    K_LOG_INFO("                          free; location is set by the current values of frameBufferX and frameBufferY.\n");
    K_LOG_INFO("                          left; next framebuffer will expand the layout to the left.\n");
    K_LOG_INFO("                          right; next framebuffer will expand the layout to the right.\n");
    K_LOG_INFO("                          top; next framebuffer will expand the layout to the top.\n");
    K_LOG_INFO("                          bottom; next framebuffer will expand the layout to the bottom.\n");
    K_LOG_INFO("-fbx, --frameBufferX num  sets the x location for the next framebuffers.\n");
    K_LOG_INFO("-fby, --frameBufferY num  sets the y location for the next framebuffers.\n");
    K_LOG_INFO("-fb, --frameBuffer path   adds a framebuffer device (like '/dev/fb0').\n");
    K_LOG_INFO("-fbng, --frameBufferNewGrps framebuffers after this wil start new fb layouts.\n");
    K_LOG_INFO("\n");
    K_LOG_INFO("-dec, --decoder path      sets the path to decoder device (like '/dev/video0') for next streams.\n");
    K_LOG_INFO("-srv, --server name/ip    sets the name/ip to server for next streams.\n");
    K_LOG_INFO("-p, --port num            sets the port number for next streams.\n");
    K_LOG_INFO("-ka, --keepAlive 0|1      sets the 'keepAlive' value for streams net-conns.\n");
    K_LOG_INFO("-s, --stream path         adds a network stream source (like '/http/relative/path/file.h.264').\n");
    K_LOG_INFO("-f, --file path           adds a file stream source (like '/file/path/file.h.264').\n");
    K_LOG_INFO("\n");
    K_LOG_INFO("-v                        same as '--logLevel verbose'.");
    K_LOG_INFO("-llvl, --logLevel v       sets the maximun log level to output:");
    K_LOG_INFO("                          critical; only assertions and critical will be printed.");
    K_LOG_INFO("                          error; errors and higher.");
    K_LOG_INFO("                          warning; warnings and higher.");
    K_LOG_INFO("                          info; info and higher (default).");
    K_LOG_INFO("                          verbose; all messages.");
    K_LOG_INFO("-lf, --logFile path       opens the file for log output.");
    K_LOG_INFO("-lfsz, --logFileMaxKB KBs activates circular-mode on the log file with that limit in KBs.");
    K_LOG_INFO("-stdout-off, --stdOutOff  skips logs to stdout.");
    K_LOG_INFO("-stderr-off, --stdErrOff  skips logs to stderr.");
    K_LOG_INFO("\n");
    K_LOG_INFO("DEBUG OPTIONS:\n");
    K_LOG_INFO("--secsRunAndExit num      seconds after starting to automatically activate stop-flag and exit, for debug and test.\n");
    K_LOG_INFO("--secsSleepBeforeExit num seconds to sleep before exiting the main() funcion, for memory leak detection.\n");
    K_LOG_INFO("--simNetworkTimeout num   (1/num) probability to trigger a simulated network timeout, for cleanup code test.\n");
    K_LOG_INFO("--simDecoderTimeout num   (1/num) probability to trigger a simulated decoder timeout, for cleanup code test.\n");
    K_LOG_INFO("--framesSkip num          ammount of frames to skip than fed to the decoder.\n");
    K_LOG_INFO("--framesFeedMax num       ammount of frames to decode and then stop.\n");
    K_LOG_INFO("\n");
}
    
int main(int argc, char* argv[]){
    int r = -1; int helpPrinted = 0, errorFatal = 0;
    int secsRunAndExit = 0, secsSleepBeforeExit = 0;
    unsigned long framesSkip = 0, framesFeedMax = 0;
    STPlayer* p = (STPlayer*)malloc(sizeof(STPlayer));
    //random initialization
    srand(time(NULL));
    //
    __logInit();
    Player_init(p);
    //defaults
    {
        p->cfg.extraThreadsAmm          = K_DEF_THREADS_EXTRA_AMM;
        p->cfg.connTimeoutSecs          = K_DEF_CONN_TIMEOUT_SECS;
        p->cfg.connWaitReconnSecs       = K_DEF_CONN_RETRY_WAIT_SECS;
        p->cfg.decoderTimeoutSecs       = K_DEF_DECODER_TIMEOUT_SECS;
        p->cfg.decoderWaitRecopenSecs   = K_DEF_DECODER_RETRY_WAIT_SECS;
        p->cfg.decodersMax              = K_DEF_DECODERS_MAX_AMM;
        p->cfg.decodersToPeekSecs       = K_DEF_DECODERS_PEEK_MAX_SECS;
        p->cfg.animSecsWaits            = K_DEF_ANIM_WAIT_SECS;
        p->cfg.animPreRenderSecs        = K_DEF_ANIM_PRE_RENDER_SECS;
        p->cfg.screenRefreshPerSec      = K_DEF_FRAMES_PER_SEC;
        p->cfg.drawMode                 = K_DEF_DRAW_MODE;
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
        int port = 0, keepAlive = 0;
        ENFramebuffsGrpFbLocation fbLoc = ENFramebuffsGrpFbLocation_Free;
        int fbLocX = 0, fbLocY = 0;
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
                    K_LOG_ERROR("clould not open '/sys/class/graphics/fbcon/cursor_blink'.\n");
                } else {
                    if(write(fd, "0", 1) != 1){
                        K_LOG_ERROR("clould not write '/sys/class/graphics/fbcon/cursor_blink'.\n");
                    } else {
                        K_LOG_INFO("Param, cursor blink disabled '/sys/class/graphics/fbcon/cursor_blink'.\n");
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
                        K_LOG_INFO("Param '--extraThreads' value is not valid: '%s'\n", val);
                    } else {
                        p->cfg.extraThreadsAmm = v;
                        K_LOG_INFO("Param '--extraThreads' value set: '%d'\n", v);
                    }
                    i++;
                }
            } else if(strcmp(arg, "-cto") == 0 || strcmp(arg, "--connTimeout") == 0){
                if((i + 1) < argc){
                    const char* val = argv[i + 1];
                    char* endPtr = NULL;
                    const long int v = strtol(val, &endPtr, 0);
                    if(*endPtr != '\0'){
                        K_LOG_INFO("Param '--connTimeout' value is not valid: '%s'\n", val);
                    } else {
                        p->cfg.connTimeoutSecs = v;
                        K_LOG_INFO("Param '--connTimeout' value set: '%d'\n", v);
                    }
                    i++;
                }
            } else if(strcmp(arg, "-crc") == 0 || strcmp(arg, "--connWaitReconnect") == 0){
                if((i + 1) < argc){
                    const char* val = argv[i + 1];
                    char* endPtr = NULL;
                    const long int v = strtol(val, &endPtr, 0);
                    if(*endPtr != '\0' || v <= 0){ //0 is not allowed
                        K_LOG_INFO("Param '--connWaitReconnect' value is not valid: '%s'\n", val);
                    } else {
                        p->cfg.connWaitReconnSecs = v;
                        K_LOG_INFO("Param '--connWaitReconnect' value set: '%d'\n", v);
                    }
                    i++;
                }
            } else if(strcmp(arg, "-dto") == 0 || strcmp(arg, "--decTimeout") == 0){
                if((i + 1) < argc){
                    const char* val = argv[i + 1];
                    char* endPtr = NULL;
                    const long int v = strtol(val, &endPtr, 0);
                    if(*endPtr != '\0'){
                        K_LOG_INFO("Param '--decTimeout' value is not valid: '%s'\n", val);
                    } else {
                        p->cfg.decoderTimeoutSecs = v;
                        K_LOG_INFO("Param '--decTimeout' value set: '%d'\n", v);
                    }
                    i++;
                }
            } else if(strcmp(arg, "-dro") == 0 || strcmp(arg, "--decWaitReopen") == 0){
                if((i + 1) < argc){
                    const char* val = argv[i + 1];
                    char* endPtr = NULL;
                    const long int v = strtol(val, &endPtr, 0);
                    if(*endPtr != '\0' || v <= 0){ //0 is not allowed
                        K_LOG_INFO("Param '--decWaitReopen' value is not valid: '%s'\n", val);
                    } else {
                        p->cfg.decoderWaitRecopenSecs = v;
                        K_LOG_INFO("Param '--decWaitReopen' value set: '%d'\n", v);
                    }
                    i++;
                }
            } else if(strcmp(arg, "-aw") == 0 || strcmp(arg, "--animWait") == 0){
                if((i + 1) < argc){
                    const char* val = argv[i + 1];
                    char* endPtr = NULL;
                    const long int v = strtol(val, &endPtr, 0);
                    if(*endPtr != '\0'){
                        K_LOG_INFO("Param '--animWait' value is not valid: '%s'\n", val);
                    } else {
                        p->cfg.animSecsWaits = v;
                        K_LOG_INFO("Param '--animWait' value set: '%d'\n", v);
                    }
                    i++;
                }
            } else if(strcmp(arg, "-fps") == 0 || strcmp(arg, "--framesPerSec") == 0){
                if((i + 1) < argc){
                    const char* val = argv[i + 1];
                    char* endPtr = NULL;
                    const long int v = strtol(val, &endPtr, 0);
                    if(*endPtr != '\0'){
                        K_LOG_INFO("Param '--framesPerSec' value is not valid: '%s'\n", val);
                    } else {
                        p->cfg.screenRefreshPerSec = v;
                        K_LOG_INFO("Param '--framesPerSec' value set: '%d'\n", v);
                    }
                    i++;
                }
            } else if(strcmp(arg, "-dm") == 0 || strcmp(arg, "--drawMode") == 0){
                if((i + 1) < argc){
                    const char* val = argv[i + 1];
                    if(strcmp(val, "src") == 0){
                        p->cfg.drawMode = ENPlayerDrawMode_Src;
                        K_LOG_INFO("Main, --drawMode: '%s'.\n", val);
                    } else if(strcmp(val, "dst") == 0){
                        p->cfg.drawMode = ENPlayerDrawMode_Dst;
                        K_LOG_INFO("Main, --drawMode: '%s'.\n", val);
                    } else {
                        K_LOG_INFO("Main, --drawMode unknown value: '%s'.\n", val);
                    }
                    i++;
                }
            } else if(strcmp(arg, "-fb") == 0 || strcmp(arg, "--frameBuffer") == 0){
                if((i + 1) < argc){
                    const char* val = argv[i + 1];
                    if(0 != Player_fbAdd(p, val, fbLoc, fbLocX, fbLocY, p->cfg.animSecsWaits)){
                        K_LOG_ERROR("main, could not add fb.\n");
                        errorFatal = 1;
                    } else {
                        K_LOG_INFO("Main, fb added: '%s'.\n", val);
                    }
                    i++;
                }
            } else if(strcmp(arg, "-fbng") == 0 || strcmp(arg, "--frameBufferNewGrps") == 0){
                if(0 != Player_fbsCloseCurrentGrps(p)){
                    K_LOG_ERROR("main, could not close fbs.\n");
                    errorFatal = 1;
                } else {
                    K_LOG_INFO("Main, current fbs layout closed.\n");
                }
            } else if(strcmp(arg, "-fbl") == 0 || strcmp(arg, "--frameBufferLoc") == 0){
                if((i + 1) < argc){
                    const char* val = argv[i + 1];
                    if(strcmp(val, "free") == 0){
                        fbLoc = ENFramebuffsGrpFbLocation_Free;
                        K_LOG_INFO("Main, --frameBufferLoc: '%s'.\n", val);
                    } else if(strcmp(val, "left") == 0){
                        fbLoc = ENFramebuffsGrpFbLocation_Left;
                        K_LOG_INFO("Main, --frameBufferLoc: '%s'.\n", val);
                    } else if(strcmp(val, "right") == 0){
                        fbLoc = ENFramebuffsGrpFbLocation_Right;
                        K_LOG_INFO("Main, --frameBufferLoc: '%s'.\n", val);
                    } else if(strcmp(val, "top") == 0){
                        fbLoc = ENFramebuffsGrpFbLocation_Top;
                        K_LOG_INFO("Main, --frameBufferLoc: '%s'.\n", val);
                    } else if(strcmp(val, "bottom") == 0){
                        fbLoc = ENFramebuffsGrpFbLocation_Bottom;
                        K_LOG_INFO("Main, --frameBufferLoc: '%s'.\n", val);
                    } else {
                        K_LOG_INFO("Main, --frameBufferLoc unknown value: '%s'.\n", val);
                    }
                    i++;
                }
            } else if(strcmp(arg, "-fbx") == 0 || strcmp(arg, "--frameBufferX") == 0){
                if((i + 1) < argc){
                    const char* val = argv[i + 1];
                    char* endPtr = NULL;
                    const long int v = strtol(val, &endPtr, 0);
                    if(*endPtr != '\0'){
                        K_LOG_INFO("Param '--frameBufferX' value is not valid: '%s'\n", val);
                    } else {
                        fbLocX = v;
                        K_LOG_INFO("Param '--frameBufferX' value set: '%d'\n", v);
                    }
                    i++;
                }
            } else if(strcmp(arg, "-fby") == 0 || strcmp(arg, "--frameBufferY") == 0){
                if((i + 1) < argc){
                    const char* val = argv[i + 1];
                    char* endPtr = NULL;
                    const long int v = strtol(val, &endPtr, 0);
                    if(*endPtr != '\0'){
                        K_LOG_INFO("Param '--frameBufferY' value is not valid: '%s'\n", val);
                    } else {
                        fbLocY = v;
                        K_LOG_INFO("Param '--frameBufferY' value set: '%d'\n", v);
                    }
                    i++;
                }
            } else if(strcmp(arg, "-dec") == 0 || strcmp(arg, "--decoder") == 0){
                if((i + 1) < argc){
                    decoder = argv[i + 1];
                    K_LOG_INFO("Param '--decoder' value set: '%s'\n", argv[i + 1]);
                    i++;
                }
            } else if(strcmp(arg, "-srv") == 0 || strcmp(arg, "--server") == 0){
                if((i + 1) < argc){
                    server = argv[i + 1];
                    K_LOG_INFO("Param '--server' value set: '%s'\n", argv[i + 1]);
                    i++;
                }
            } else if(strcmp(arg, "-p") == 0 || strcmp(arg, "--port") == 0){
                if((i + 1) < argc){
                    const char* val = argv[i + 1];
                    char* endPtr = NULL;
                    const long int v = strtol(val, &endPtr, 0);
                    if(*endPtr != '\0'){
                        K_LOG_INFO("Param '--port' value is not valid: '%s'\n", val);
                    } else {
                        port = v;
                        K_LOG_INFO("Param '--port' value set: '%d'\n", v);
                    }
                    i++;
                }
            } else if(strcmp(arg, "-ka") == 0 || strcmp(arg, "--keepAlive") == 0){
                if((i + 1) < argc){
                    const char* val = argv[i + 1];
                    char* endPtr = NULL;
                    const long int v = strtol(val, &endPtr, 0);
                    if(*endPtr != '\0' || v != 0 || v != 1){
                        K_LOG_INFO("Param '--keepAlive' value is not valid: '%s'\n", val);
                    } else {
                        keepAlive = v;
                        K_LOG_INFO("Param '--keepAlive' value set: '%d'\n", v);
                    }
                    i++;
                }
            } else if(strcmp(arg, "-s") == 0 || strcmp(arg, "--stream") == 0){
                if((i + 1) < argc){
                    const char* val = argv[i + 1];
                    if(decoder == NULL){
                        K_LOG_ERROR("param '--stream' missing previous param: '--decoder'.\n");
                        errorFatal = 1;
                    } else if(server == NULL){
                        K_LOG_ERROR("param '--stream' missing previous param: '--server'.\n");
                        errorFatal = 1;
                    } else if(port <= 0){
                        K_LOG_ERROR("param '--stream' missing previous param: '--port'.\n");
                        errorFatal = 1;
                    } else if(0 != Player_streamAdd(p, decoder, server, port, keepAlive, val, p->cfg.connTimeoutSecs, p->cfg.decoderTimeoutSecs, framesSkip, framesFeedMax)){
                        K_LOG_ERROR("main, could not add stream: '%s'.\n", val);
                        errorFatal = 1;
                    } else {
                        K_LOG_INFO("Main, stream added: '%s'.\n", val);
                    }
                    i++;
                }
            } else if(strcmp(arg, "-f") == 0 || strcmp(arg, "--file") == 0){
                if((i + 1) < argc){
                    const char* val = argv[i + 1];
                    if(decoder == NULL){
                        K_LOG_ERROR("param '--file' missing previous param: '--decoder'.\n");
                        errorFatal = 1;
                    } else if(0 != Player_streamAdd(p, decoder, NULL, 0, keepAlive, val, p->cfg.connTimeoutSecs, p->cfg.decoderTimeoutSecs, framesSkip, framesFeedMax)){
                        K_LOG_ERROR("main, could not add stream: '%s'.\n", val);
                        errorFatal = 1;
                    } else {
                        K_LOG_INFO("Main, stream added: '%s'.\n", val);
                    }
                    i++;
                }
            } else if(strcmp(arg, "-llvl") == 0 || strcmp(arg, "--logLevel") == 0){
                if((i + 1) < argc){
                    const char* val = argv[i + 1];
                    if(strcmp(val, "critical") == 0){
                        _logLvlMax = ENLogLevel_Critical;
                        K_LOG_INFO("Main, --logLevel: '%s'.\n", val);
                    } else if(strcmp(val, "error") == 0){
                        _logLvlMax = ENLogLevel_Error;
                        K_LOG_INFO("Main, --logLevel: '%s'.\n", val);
                    } else if(strcmp(val, "warning") == 0){
                        _logLvlMax = ENLogLevel_Warning;
                        K_LOG_INFO("Main, --logLevel: '%s'.\n", val);
                    } else if(strcmp(val, "info") == 0){
                        _logLvlMax = ENLogLevel_Info;
                        K_LOG_INFO("Main, --logLevel: '%s'.\n", val);
                    } else if(strcmp(val, "verbose") == 0){
                        _logLvlMax = ENLogLevel_Verbose;
                        K_LOG_INFO("Main, --logLevel: '%s'.\n", val);
                    } else {
                        K_LOG_INFO("Main, --logLevel unknown value: '%s'.\n", val);
                    }
                    i++;
                }
            } else if(strcmp(arg, "-v") == 0){
                _logLvlMax = ENLogLevel_Verbose;
                K_LOG_INFO("Main, --logLevel: 'verbose'.\n");
            } else if(strcmp(arg, "-lf") == 0 || strcmp(arg, "--logFile") == 0){
                if((i + 1) < argc){
                    const char* val = argv[i + 1];
                    if(0 != __logOpenFile(val)){
                        K_LOG_ERROR("Main, --logFile __logOpenFile failed: '%s'.\n", val);
                    } else {
                        K_LOG_INFO("Main, --logFile opened: '%s'.\n", val);
                    }
                    i++;
                }
            } else if(strcmp(arg, "-lfsz") == 0 || strcmp(arg, "--logFileMaxKB") == 0){
                if((i + 1) < argc){
                    const char* val = argv[i + 1];
                    char* endPtr = NULL;
                    const long int v = strtol(val, &endPtr, 0);
                    if(*endPtr != '\0'){
                        K_LOG_INFO("Param '--logFileMaxKB' value is not valid: '%s'\n", val);
                    } else {
                        _logStreamMaxSz = (v * 1024);
                        K_LOG_INFO("Param '--logFileMaxKB' value set: '%d'\n", v);
                    }
                    i++;
                }
            } else if(strcmp(arg, "-stdout-off") == 0 || strcmp(arg, "--stdOutOff") == 0){
                K_LOG_INFO("Param '--stdOutOff' skipping stdout logs.\n");
                _logStdOutOff = 1;
            } else if(strcmp(arg, "-stderr-off") == 0 || strcmp(arg, "--stdErrOff") == 0){
                K_LOG_INFO("Param '--stdErrOff' skipping stderr logs.\n");
                _logStdErrOff = 1;
            } else if(strcmp(arg, "--secsRunAndExit") == 0){
                if((i + 1) < argc){
                    const char* val = argv[i + 1];
                    char* endPtr = NULL;
                    const long int v = strtol(val, &endPtr, 0);
                    if(*endPtr != '\0' || v < 0){
                        K_LOG_INFO("Param '--secsRunAndExit' value is not valid: '%s'\n", val);
                    } else {
                        secsRunAndExit = v;
                        K_LOG_INFO("Param '--secsRunAndExit' value set: '%d'\n", v);
                    }
                    i++;
                }
            } else if(strcmp(arg, "--secsSleepBeforeExit") == 0){
                if((i + 1) < argc){
                    const char* val = argv[i + 1];
                    char* endPtr = NULL;
                    const long int v = strtol(val, &endPtr, 0);
                    if(*endPtr != '\0' || v < 0){
                        K_LOG_INFO("Param '--secsSleepBeforeExit' value is not valid: '%s'\n", val);
                    } else {
                        secsSleepBeforeExit = v;
                        K_LOG_INFO("Param '--secsSleepBeforeExit' value set: '%d'\n", v);
                    }
                    i++;
                }
            } else if(strcmp(arg, "--simNetworkTimeout") == 0){
                if((i + 1) < argc){
                    const char* val = argv[i + 1];
                    char* endPtr = NULL;
                    const long int v = strtol(val, &endPtr, 0);
                    if(*endPtr != '\0' || v < 0){
                        K_LOG_INFO("Param '--simNetworkTimeout' value is not valid: '%s'\n", val);
                    } else {
                        p->cfg.dbg.simNetworkTimeout = v;
                        K_LOG_INFO("Param '--simNetworkTimeout' value set: '%d'\n", v);
                    }
                    i++;
                }
            } else if(strcmp(arg, "--simDecoderTimeout") == 0){
                if((i + 1) < argc){
                    const char* val = argv[i + 1];
                    char* endPtr = NULL;
                    const long int v = strtol(val, &endPtr, 0);
                    if(*endPtr != '\0' || v < 0){
                        K_LOG_INFO("Param '--simDecoderTimeout' value is not valid: '%s'\n", val);
                    } else {
                        p->cfg.dbg.simDecoderTimeout = v;
                        K_LOG_INFO("Param '--simDecoderTimeout' value set: '%d'\n", v);
                    }
                    i++;
                }
            } else if(strcmp(arg, "--framesSkip") == 0){
                if((i + 1) < argc){
                    const char* val = argv[i + 1];
                    char* endPtr = NULL;
                    const long int v = strtol(val, &endPtr, 0);
                    if(*endPtr != '\0' || v < 0){
                        K_LOG_INFO("Param '--framesSkip' value is not valid: '%s'\n", val);
                    } else {
                        framesSkip = v;
                        K_LOG_INFO("Param '--framesSkip' value set: '%d'\n", v);
                    }
                    i++;
                }
            } else if(strcmp(arg, "--framesFeedMax") == 0){
                if((i + 1) < argc){
                    const char* val = argv[i + 1];
                    char* endPtr = NULL;
                    const long int v = strtol(val, &endPtr, 0);
                    if(*endPtr != '\0' || v < 0){
                        K_LOG_INFO("Param '--framesFeedMax' value is not valid: '%s'\n", val);
                    } else {
                        framesFeedMax = v;
                        K_LOG_INFO("Param '--framesFeedMax' value set: '%d'\n", v);
                    }
                    i++;
                }
            }
        }
    }
    //execute
    if(!errorFatal){
        if(p->streams.arrUse <= 0){
            K_LOG_INFO("Main, no streams loaded.\n");
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
            r = 0;
            //start extra threads
            if(r == 0 && p->cfg.extraThreadsAmm > 0){
                if(0 != Player_createExtraThreads(p, p->cfg.extraThreadsAmm)){
                    K_LOG_ERROR("Main, Player_createExtraThreads(%d) failed.\n", p->cfg.extraThreadsAmm);
                    r = -1;
                }
            }
            //cycle
            if(r == 0){
                int countStreamsPermShuttedDown = 0;
                while(countStreamsPermShuttedDown < p->streams.arrUse){
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
                                K_LOG_VERBOSE("Main, fd-poll-autoremoved.\n");
                            }
                        }
                        //reset
                        p->poll.autoremovesPend = 0;
                    }
                    //poll
                    if(p->poll.fdsUse <= 0){
                        //just sleep
                        int mSecs = (animMsPerFrame / 4);
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
                                        const char* typeStr = "unknow-type";
                                        switch (fdd->type) {
                                            case ENPlayerPollFdType_Decoder: typeStr = "decoder"; break;
                                            case ENPlayerPollFdType_SrcFile: typeStr = "file-fd"; break;
                                            case ENPlayerPollFdType_SrcSocket: typeStr = "net-socket"; break;
                                            default: typeStr = "unknow-type"; break;
                                        }
                                        K_LOG_VERBOSE("Main, %s poll: %s%s%s%s%s%s.\n", typeStr, (fd->revents & POLLOUT ? " POLLOUT" : ""), (fd->revents & POLLWRNORM ? " POLLWRNORM" : ""), (fd->revents & POLLIN ? " POLLIN" : ""), (fd->revents & POLLRDNORM ? " POLLRDNORM" : ""), (fd->revents & POLLERR ? " POLLERR" : ""), (fd->revents & POLLPRI ? " POLLPRI" : ""));
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
                        const long msAccum = (ms + animMsAccum);
                        if(ms > 0 && msAccum >= animMsPerFrame){
                            if(0 != Player_tick(p, animMsPerFrame)){
                                K_LOG_ERROR("Main, anim-tick fail.\n");
                            }
                            animMsAccum = (ms + animMsAccum) % animMsPerFrame;
                            K_LOG_VERBOSE("Main, anim-tick (%dms passed, %dms tick, %dms remain).\n", msAccum, animMsPerFrame, animMsAccum);
                            animPrev = animCur;
                        }
                    }
                    //time passed
                    gettimeofday(&timeCur, NULL);
                    long ms = msBetweenTimevals(&timePrev, &timeCur);
                    if(ms >= 1000){
                        //stats
                        {
                            int toDrawCountTotal = 0, decsCountTotal = 0, peekCountTotal = 0, netsTotal = 0, filesTotal = 0;
                            //count draw plan hits after final draw
                            {
                                int i; for(i = 0; i < p->streams.arrUse; i++){
                                    const STStreamContext* s = p->streams.arr[i];
                                    if(s->drawPlan.hitsCount > 0){
                                        toDrawCountTotal++;
                                    }
                                    if(s->drawPlan.peekRemainMs > 0){
                                        peekCountTotal++;
                                    }
                                    if(s->dec.fd >= 0){
                                        decsCountTotal++;
                                    }
                                    if(s->file.fd > 0){
                                        filesTotal++;
                                    }
                                    if(s->net.hostResolver != NULL || s->net.socket > 0){
                                        netsTotal++;
                                    }
                                }
                            }
                            pthread_mutex_lock(&p->stats.mutex);
                            {
                                if(p->stats.curSec.draw.count <= 0){
                                    K_LOG_INFO("Main, sec: %d streams, %d/%d decs (%d peek), file(%d)-net(%d), frame[%d qued, %d IDR, %d ign, %u fed dec(%u, %ums/%ums/%ums, %d skipped)], drawn(%u).\n", p->streams.arrUse
                                               , decsCountTotal, toDrawCountTotal, peekCountTotal
                                               , filesTotal, netsTotal
                                               , p->stats.curSec.src.frames.queued, p->stats.curSec.src.frames.queuedIDR, p->stats.curSec.src.frames.ignored
                                               , p->stats.curSec.dec.fed.count, p->stats.curSec.dec.got.count, p->stats.curSec.dec.got.msMin, (p->stats.curSec.dec.got.count <= 0 ? 0 : p->stats.curSec.dec.got.msSum / p->stats.curSec.dec.got.count), p->stats.curSec.dec.got.msMax, p->stats.curSec.dec.got.skipped
                                               , p->stats.curSec.draw.count
                                               );
                                } else if(p->stats.curSec.draw.msMin <= 0){
                                    K_LOG_INFO("Main, sec: %d streams, %d/%d decs (%d peek), file(%d)-net(%d), frame[%d qued, %d IDR, %d ign, %u fed, dec(%u, %ums/%ums/%ums, %d skipped)], drawn(%u, %u/%u/%u ms).\n", p->streams.arrUse
                                               , decsCountTotal, toDrawCountTotal, peekCountTotal
                                               , filesTotal, netsTotal
                                               , p->stats.curSec.src.frames.queued, p->stats.curSec.src.frames.queuedIDR, p->stats.curSec.src.frames.ignored
                                               , p->stats.curSec.dec.fed.count, p->stats.curSec.dec.got.count, p->stats.curSec.dec.got.msMin, (p->stats.curSec.dec.got.count <= 0 ? 0 : p->stats.curSec.dec.got.msSum / p->stats.curSec.dec.got.count), p->stats.curSec.dec.got.msMax, p->stats.curSec.dec.got.skipped
                                               , p->stats.curSec.draw.count, p->stats.curSec.draw.msMin, p->stats.curSec.draw.msSum / p->stats.curSec.draw.count, p->stats.curSec.draw.msMax
                                               );
                                } else {
                                    K_LOG_INFO("Main, sec: %d streams, %d/%d decs (%d peek), file(%d)-net(%d), frame[%d qued, %d IDR, %d ign, %u fed, dec(%u, %ums/%ums/%ums, %d skipped)], drawn(%u, %u/%u/%u ms, %u/%u/%u fps max).\n", p->streams.arrUse
                                               , decsCountTotal, toDrawCountTotal, peekCountTotal
                                               , filesTotal, netsTotal
                                               , p->stats.curSec.src.frames.queued, p->stats.curSec.src.frames.queuedIDR, p->stats.curSec.src.frames.ignored
                                               , p->stats.curSec.dec.fed.count, p->stats.curSec.dec.got.count, p->stats.curSec.dec.got.msMin, (p->stats.curSec.dec.got.count <= 0 ? 0 : p->stats.curSec.dec.got.msSum / p->stats.curSec.dec.got.count), p->stats.curSec.dec.got.msMax, p->stats.curSec.dec.got.skipped
                                               , p->stats.curSec.draw.count, p->stats.curSec.draw.msMin, p->stats.curSec.draw.msSum / p->stats.curSec.draw.count, p->stats.curSec.draw.msMax, 1000ULL / p->stats.curSec.draw.msMax, 1000ULL / (p->stats.curSec.draw.msSum / p->stats.curSec.draw.count), 1000ULL / p->stats.curSec.draw.msMin
                                               );
                                }
                                //reset
                                memset(&p->stats.curSec, 0, sizeof(p->stats.curSec));
                            }
                            pthread_mutex_unlock(&p->stats.mutex);
                        }
                        timePrev = timeCur;
                        secsRunnning++;
                    }
                    //analyze streams (shutdown permanently)
                    {
                        countStreamsPermShuttedDown = 0;
                        int i; for(i = (int)p->streams.arrUse - 1; i >= 0; i--){
                            STStreamContext* ctx = p->streams.arr[i];
                            //count
                            if(ctx->shuttingDown.isActive && ctx->shuttingDown.isCompleted && ctx->shuttingDown.isPermanent){
                                countStreamsPermShuttedDown++;
                            } else {
                                int shouldBePermShuttDown = 0;
                                const char* reason = "";
                                //reasons to shutdown streams
                                if(__stopInterrupt){
                                    shouldBePermShuttDown = 1;
                                    reason = "interrupt-activated";
                                } else if(secsRunAndExit > 0 && secsRunAndExit == secsRunnning){
                                    shouldBePermShuttDown = 1;
                                    reason = "secs-running-limit-reached";
                                }
                                //
                                if(shouldBePermShuttDown){
                                    const int isPermanent = 1;
                                    if(ctx->shuttingDown.isActive){
                                        if(!ctx->shuttingDown.isCompleted && !ctx->shuttingDown.isPermanent){
                                            ctx->shuttingDown.isPermanent = isPermanent;
                                            K_LOG_INFO("Player, StreamContext current shutdown flagged (at tick, '%s').\n", reason);
                                        }
                                    } else if(!ctx->shuttingDown.isCompleted && !ctx->shuttingDown.isPermanent){
                                        if(0 != StreamContext_shutdownStart(ctx, p, isPermanent)){
                                            K_LOG_ERROR("Player, StreamContext_shutdownStart failed (at tick, '%s').\n", reason);
                                        } else {
                                            //K_LOG_INFO("Player, StreamContext_shutdownStart shutdown started (at tick, '%s').\n", reason);
                                        }
                                    }
                                }
                                
                            }
                        }
                    }
                    //
                }
            }
        }
    }
    if(__stopInterrupt){
        K_LOG_INFO("Main, ending (stop-interrupted)...\n");
    } else {
        K_LOG_INFO("Main, ending...\n");
    }
    if(p != NULL){
        Player_release(p);
        free(p);
        p = NULL;
    }
    if(__stopInterrupt){
        K_LOG_INFO("Main, ended (stop-interrupted).\n");
    } else {
        K_LOG_INFO("Main, ended.\n");
    }
    __logEnd();
    //
    if(secsSleepBeforeExit > 0){
        unsigned long s = 0;
        while(s < secsSleepBeforeExit){
            K_LOG_INFO("Main, waiting %u/%u secs before exiting main().\n", (s + 1), secsSleepBeforeExit);
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
    //draw
    {
        pthread_mutex_init(&obj->draw.mutex, NULL);
        pthread_cond_init(&obj->draw.cond, NULL);

    }
}

void Player_release(STPlayer* obj){
    //draw
    {
        pthread_mutex_lock(&obj->draw.mutex);
        {
            K_ASSERT(obj->draw.tasksPendCount == 0);
            while(obj->draw.tasksPendCount > 0){
                pthread_cond_wait(&obj->draw.cond, &obj->draw.mutex);
            }
        }
        pthread_mutex_unlock(&obj->draw.mutex);
        pthread_cond_destroy(&obj->draw.cond);
        pthread_mutex_destroy(&obj->draw.mutex);
    }
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
        //grps
        {
            if(obj->fbs.grps.arr != NULL){
                int i; for(i = 0; i < obj->fbs.grps.use; i++){
                    FramebuffsGrp_release(&obj->fbs.grps.arr[i]);
                }
                free(obj->fbs.grps.arr);
                obj->fbs.grps.arr = NULL;
            }
            obj->fbs.grps.use = 0;
            obj->fbs.grps.sz = 0;
        }
        //
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
            K_LOG_VERBOSE("Player, print('%s' : %d : %d): %d secs ago (IGNORING).\n", device, srcFmt, dstFmt, (ms / 1000));
            r = NULL;
        } else {
            K_LOG_VERBOSE("Player, print('%s' : %d : %d): %d secs ago (RETURNING).\n", device, srcFmt, dstFmt, (ms / 1000));
        }
    } else {
        K_LOG_VERBOSE("Player, print('%s' : %d : %d): n-secs ago (NEVER TOUCHED).\n", device, srcFmt, dstFmt);
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
                        K_LOG_INFO("Player_createExtraThreads, Thread_start failed.\n");
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
    int r = -1, fnd = 0;
    //search
    int i; for(i = 0; i < obj->poll.fdsUse; i++){
        STPlayerPollFd* fdsN = &obj->poll.fds[i];
        struct pollfd* fdsNatN = &obj->poll.fdsNat[i];
        if(fdsN->type == type && fdsN->obj == objPtr && fdsNatN->fd == fd){
            fnd = 1;
            break;
        }
    }
    if(!fnd){
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

int Player_fbAdd(STPlayer* obj, const char* device, const ENFramebuffsGrpFbLocation location, const int locX, const int locY, const int animSecsWaits){
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
        if(0 != Framebuff_open(fbN, device)){
            K_LOG_ERROR("Player, fbAdd failed: '%s'.\n", device);
            r = -1;
        } else {
            K_LOG_INFO("Player_fbAdd device added to poll: '%s'.\n", device);
            //consume
            obj->fbs.arr[obj->fbs.arrUse] = fbN;
            obj->fbs.arrUse++;
            //Add to group (y pixFmt)
            {
                STFramebuffsGrp* grpFnd = NULL;
                //search a match
                if(obj->fbs.grps.arr != NULL){
                    int i; for(i = 0; i < obj->fbs.grps.use; i++){
                        STFramebuffsGrp* grp = &obj->fbs.grps.arr[i];
                        if(!grp->isClosed && grp->pixFmt == fbN->pixFmt){
                            grpFnd = grp;
                            break;
                        }
                    }
                }
                //allocate (if not found)
                if(grpFnd == NULL){
                    //resize
                    while(obj->fbs.grps.use >= obj->fbs.grps.sz){
                        const int szN = obj->fbs.grps.sz + 8;
                        STFramebuffsGrp* arrN = (STFramebuffsGrp*)malloc(sizeof(obj->fbs.grps.arr[0]) * szN);
                        if(arrN == NULL){
                            break;
                        } else {
                            if(obj->fbs.grps.arr != NULL){
                                if(obj->fbs.grps.use > 0){
                                    memcpy(arrN, obj->fbs.grps.arr, sizeof(obj->fbs.grps.arr[0]) * obj->fbs.grps.use);
                                }
                                free(obj->fbs.grps.arr);
                            }
                            obj->fbs.grps.arr = arrN;
                            obj->fbs.grps.sz = szN;
                        }
                    }
                    //add
                    if(obj->fbs.grps.use < obj->fbs.grps.sz){
                        grpFnd = &obj->fbs.grps.arr[obj->fbs.grps.use++];
                        FramebuffsGrp_init(grpFnd);
                        grpFnd->pixFmt = fbN->pixFmt;
                        grpFnd->cfg.animSecsWaits = animSecsWaits;
                    }
                }
                //add
                if(grpFnd == NULL){
                    K_LOG_ERROR("Player, fbAdd failed, could not create fbsGrp: '%s'.\n", device);
                    r = -1;
                } else if(0 != FramebuffsGrp_addFb(grpFnd, fbN, location, locX, locY)){ //ToDo: implement 'ENFramebuffsGrpFbLocation_'
                    K_LOG_ERROR("Player, fbAdd failed, could not add to fbsGrp: '%s'.\n", device);
                    r = -1;
                } else {
                    K_LOG_INFO("Player, fbAdd opened and to fbsGrp: '%s'.\n", device);
                }
            }
            //reorganize
            if(0 != Player_organize(obj)){
                K_LOG_ERROR("Player_organize failed after fb creation.\n");
            }
            //consume
            fbN = NULL;
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
                K_LOG_ERROR("Player_organize failed after fb removal.\n");
            }
            break;
        }
    }
    return r;
}

int Player_fbsCloseCurrentGrps(STPlayer* obj){
    int i; for(i = 0; i < obj->fbs.grps.use; i++){
        STFramebuffsGrp* grp = &obj->fbs.grps.arr[i];
        grp->isClosed = 1;
    }
    return 0;
}

//streams

int Player_streamAdd(STPlayer* obj, const char* device, const char* server, const unsigned int port, const int keepAlive, const char* resPath, const int connTimeoutSecs, const int decoderTimeoutSecs, const unsigned long framesSkip, const unsigned long framesFeedMax){
    int r = -1;
    if(resPath == NULL || resPath[0] == '\0'){
        K_LOG_ERROR("Player_streamAdd 'resPath' is required.\n");
    } else {
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
            int fileFd = -1;
            const int isFileFd = ((server == NULL || server[0] == '\0') && port <= 0) ? 1 : 0;
            if(isFileFd){
                //stream src is file
                fileFd = open(resPath, O_RDONLY);
                if(fileFd < 0){
                    K_LOG_ERROR("Player, streamAdd, open failed: '%s'.\n", resPath);
                }
            }
            if(isFileFd && fileFd < 0){
                K_LOG_ERROR("Player, streamAdd, file is required to add: '%s'.\n", resPath);
            } else {
                //add only once, trying every known dstPixFmt (will be validated in the open method).
                int lastPixFmt = 0, fmtsCount = 0, streamAdded = 0;
                int i; for(i = 0; i < obj->fbs.grps.use; i++){
                    STFramebuffsGrp* grp = &obj->fbs.grps.arr[i];
                    if(!grp->isClosed && lastPixFmt != grp->pixFmt){
                        STStreamContext* streamF = NULL;
                        //find already added matching stream
                        {
                            int i; for(i = 0; i < obj->streams.arrUse; i++){
                                STStreamContext* s = obj->streams.arr[i];
                                if(s != NULL){
                                    if(0 == StreamContext_isSame(s, device, server, port, resPath, V4L2_PIX_FMT_H264, grp->pixFmt)){
                                        streamF = s;
                                        break;
                                    }
                                }
                            }
                        }
                        //
                        if(streamF != NULL){
                            //add existing stream
                            if(0 != FramebuffsGrp_addStream(grp, streamF)){
                                K_LOG_ERROR("Player, streamAdd, FramebuffsGrp_addStream failed for existing stream.\n");
                            } else {
                                K_LOG_VERBOSE("Player, streamAdd, Player_streamAdd existing stream added: '%s'.\n", resPath);
                                streamAdded++;
                                r = 0;
                            }
                        } else {
                            //open new stream
                            STStreamContext* streamN = malloc(sizeof(STStreamContext));
                            StreamContext_init(streamN);
                            if(0 != StreamContext_open(streamN, obj, device, server, port, keepAlive, resPath, V4L2_PIX_FMT_H264, 1, 1, (1024 * 1024 * 1), grp->pixFmt, connTimeoutSecs, decoderTimeoutSecs, framesSkip, framesFeedMax)){
                                //do not print.
                            } else if(0 != StreamContext_close(streamN, obj)){
                                K_LOG_ERROR("Player, streamAdd, StreamContext_close failed after StreamContext_open: '%s'.\n", resPath);
                            } else {
                                //add new buffer
                                if(0 != FramebuffsGrp_addStream(grp, streamN)){
                                    K_LOG_ERROR("Player, streamAdd, FramebuffsGrp_addStream failed for new stream.\n");
                                } else {
                                    K_LOG_VERBOSE("Player, streamAdd, Player_streamAdd device opened, closed and added: '%s'.\n", resPath);
                                    streamN->streamId = ++obj->streamIdNext;
                                    obj->streams.arr[obj->streams.arrUse] = streamN; streamN = NULL; //consume
                                    obj->streams.arrUse++;
                                    streamAdded++;
                                    r = 0;
                                }
                            }
                            //release (if not consumed)
                            if(streamN != NULL){
                                StreamContext_release(streamN);
                                free(streamN);
                                streamN = NULL;
                            }
                        }
                        //
                        lastPixFmt = grp->pixFmt;
                        fmtsCount++;
                    }
                }
                //
                if(!streamAdded){
                    if(fmtsCount == 0){
                        K_LOG_INFO("Player_streamAdd, no open fbGrp to add stream, for '%s'.\n", fmtsCount, resPath);
                    } else {
                        K_LOG_INFO("Player_streamAdd, could not add to device supporting the dstPixFmt (%d fmts found) for '%s'.\n", fmtsCount, resPath);
                    }
                    r = -1;
                } else {
                    if(0 != Player_organize(obj)){
                        K_LOG_ERROR("Player, streamAdd, Player_organize failed after new stream.\n");
                    }
                }
            }
            //release
            if(fileFd > 0){
                close(fileFd);
                fileFd = -1;
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
                K_LOG_ERROR("Player_organize failed after stream removal.\n");
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
    int i; for(i = 0 ; i < obj->fbs.grps.use; i++){
        STFramebuffsGrp* grp = &obj->fbs.grps.arr[i];
        if(0 != FramebuffsGrp_layoutStart(grp)){
            K_LOG_ERROR("FramebuffsGrp_layoutStart failed.\n");
        } else {
            int j; for(j = 0; j < grp->streams.use; j++){
                STStreamContext* s = grp->streams.arr[j];
                if(s->drawPlan.lastPixelformat == grp->pixFmt){
                    STFbSize sz;
                    memset(&sz, 0, sizeof(sz));
                    sz.width = s->drawPlan.lastCompRect.width;
                    sz.height = s->drawPlan.lastCompRect.height;
                    if(0 != FramebuffsGrp_layoutAdd(grp, s->streamId, sz)){
                        K_LOG_ERROR("FramebuffsGrp_layoutAdd failed.\n");
                    }
                }
            }
            //
            if(0 != FramebuffsGrp_layoutEnd(grp)){
                K_LOG_ERROR("FramebuffsGrp_layoutEnd failed.\n");
            }
        }
    }
    return r;
}


int Player_draw__(STPlayer* obj, STFramebuffDrawRect* rects, int rectsSz, int* dstRectsUse, int* dstLinesReq);
int Player_draw_(STPlayer* obj);

int Player_tick(STPlayer* obj, int ms){
    int r = 0;
    //
    obj->anim.tickSeq++;
    //fbs
    {
        int hitsCountOnceAnimEnd = 0, hitsCountOnceDrawEnd = 0;
        //reset draw plan hits and decrease peekers time
        {
            int i; for(i = 0; i < obj->streams.arrUse; i++){
                STStreamContext* s = obj->streams.arr[i];
                s->drawPlan.hitsCount = 0;
                //peeking image-size/stream-props (allowing decoder)
                {
                    if(s->drawPlan.lastCompRect.width > 0 && s->drawPlan.lastCompRect.height > 0){
                        if(s->drawPlan.peekRemainMs > 0){
                            //peek completed (image-size determinated)
                            s->drawPlan.peekRemainMs = 0;
                        }
                    } else {
                        s->drawPlan.peekRemainMs -= (s->drawPlan.peekRemainMs <= ms ? s->drawPlan.peekRemainMs : ms);
                    }
                }
            }
        }
        //animate fbs
        {
            const int doDrawPlanIfAnimating = 1;
            int i; for(i = 0 ; i < obj->fbs.grps.use; i++){
                STFramebuffsGrp* grp = &obj->fbs.grps.arr[i];
                if(0 != FramebuffsGrp_layoutAnimTick(grp, ms, obj, (1000 * obj->cfg.animPreRenderSecs), doDrawPlanIfAnimating)){
                    K_LOG_ERROR("Player, FramebuffsGrp_layoutAnimTick failed.\n");
                }
            }
        }
        //count draw plan hits for animations final stage
        {
            int i; for(i = 0; i < obj->streams.arrUse; i++){
                const STStreamContext* s = obj->streams.arr[i];
                if(s->drawPlan.hitsCount > 0){
                    hitsCountOnceAnimEnd++;
                }
            }
        }
        //draw
        if(0 != Player_draw_(obj)){
            K_LOG_INFO("Player, anim-draw fail.\n");
        }
        //count draw plan hits after final draw
        {
            int i; for(i = 0; i < obj->streams.arrUse; i++){
                const STStreamContext* s = obj->streams.arr[i];
                if(s->drawPlan.hitsCount > 0){
                    hitsCountOnceDrawEnd++;
                }
            }
        }
        //activate peeking
        {
            int decodersToBeOpenCount = 0;
            //Reset decoders flags and count decoders already open
            {
                int i; for(i = 0; i < obj->streams.arrUse; i++){
                    STStreamContext* s = obj->streams.arr[i];
                    s->dec.shouldBeOpen = 0;
                    if(s->dec.fd >= 0){
                        decodersToBeOpenCount++;
                        s->dec.shouldBeOpen = STREAM_CONTEXT_DECODER_SHOULD_BE_OPEN(s);
                    }
                }
            }
            //Activate new decoders that are necesary for rendering
            {
                int i; for(i = 0; i < obj->streams.arrUse; i++){
                    STStreamContext* s = obj->streams.arr[i];
                    if(s->dec.fd < 0 && !s->dec.shouldBeOpen && decodersToBeOpenCount < obj->cfg.decodersMax && STREAM_CONTEXT_DECODER_SHOULD_BE_OPEN(s) && s->drawPlan.hitsCount > 0){
                        s->dec.shouldBeOpen = 1;
                        decodersToBeOpenCount++;
                    }
                }
            }
            //Activate new decoders that are already flagged for peeking
            {
                int i; for(i = 0; i < obj->streams.arrUse; i++){
                    STStreamContext* s = obj->streams.arr[i];
                    if(s->dec.fd < 0 && !s->dec.shouldBeOpen && decodersToBeOpenCount < obj->cfg.decodersMax && STREAM_CONTEXT_DECODER_SHOULD_BE_OPEN(s) && s->drawPlan.peekRemainMs > 0){
                        s->dec.shouldBeOpen = 1;
                        decodersToBeOpenCount++;
                    }
                }
            }
            //Activate new decoders that can be flagged as new peekkers
            if(obj->cfg.decodersToPeekSecs > 0){
                int evalsInSeq = 0;
                while(evalsInSeq < obj->streams.arrUse && decodersToBeOpenCount < obj->cfg.decodersMax) {
                    //eval
                    const int iStream = (obj->peek.iNextStreamEval % obj->streams.arrUse);
                    STStreamContext* s = obj->streams.arr[iStream];
                    if(
                       s->drawPlan.lastCompRect.width <= 0 || s->drawPlan.lastCompRect.height <= 0 //image-size not determined yet
                       && s->frames.filled.use > 0 //frame(s) available to decode
                       && s->drawPlan.peekRemainMs <= 0 //not currently peeking
                       //
                       && s->dec.fd < 0 //no decoder
                       && !s->dec.shouldBeOpen //not flagged to open yet
                       )
                    {
                        s->drawPlan.peekRemainMs = (1000 * obj->cfg.decodersToPeekSecs);
                        //allow new peeker
                        if(STREAM_CONTEXT_DECODER_SHOULD_BE_OPEN(s)){ //just in case (valdate again)
                            s->dec.shouldBeOpen = 1;
                            decodersToBeOpenCount++;
                            evalsInSeq = 0; //restart cycle
                        }
                    }
                    //next
                    obj->peek.iNextStreamEval = (obj->peek.iNextStreamEval + 1) % obj->streams.arrUse;
                    evalsInSeq++;
                }
            }
        }
        K_LOG_VERBOSE("Player, draw, active streams: +%d by animation, +%d (of %d) by final-draw.\n", hitsCountOnceAnimEnd, (hitsCountOnceDrawEnd - hitsCountOnceAnimEnd), obj->streams.arrUse);
    }
    //streams
    {
        int i; for(i = (int)obj->streams.arrUse - 1; i >= 0; i--){
            STStreamContext* ctx = obj->streams.arr[i];
            StreamContext_tick(ctx, obj, ms);
        }
    }
    obj->msRunning += ms;
    return r;
}

int Player_drawGetRects_(STPlayer* obj, STFramebuffDrawRect* rects, int rectsSz, int* dstRectsUse){
    int r = 0;
    //fbs
    {
        int i; for(i = 0 ; i < obj->fbs.grps.use; i++){
            STFramebuffsGrp* grp = &obj->fbs.grps.arr[i];
            if(0 != FramebuffsGrp_drawGetRects(grp, obj, grp->layout.anim.yOffset, rects, rectsSz, dstRectsUse)){
                //do not stop, continue processing, 'dstRectsUse' could be updated by next calls.
                r = -1;
            }
        }
    }
    return r;
}

int Player_drawGetLines_(STPlayer* obj, STFramebuffDrawRect* rects, const int rectsUse, STFramebuffDrawLine* lines, const int linesSz, int* dstLinesUse){
    int r = 0;
    //build draw plan (lines per row)
    STFramebuff* rowStartFb = NULL;
    int i, iRow = -1, rowStartRectIdx = 0, rowRectsCount = 0;
    for(i = 0; i < rectsUse; i++){
        STFramebuffDrawRect* rect = &rects[i];
        if(rowStartFb != rect->fb || iRow != rect->iRow){
            if(rowRectsCount > 0){
                if(0 != Framebuff_drawRowsBuildPlan(rowStartFb, &rowStartFb->screen, &rects[rowStartRectIdx], rowRectsCount, lines, linesSz, dstLinesUse)){
                    r = -1;
                }
            }
            rowStartFb = rect->fb;
            iRow = rect->iRow;
            rowStartRectIdx = i;
            rowRectsCount = 0;
        }
        //add to row
        rowRectsCount++;
    }
    //flush last row
    if(rowRectsCount > 0){
        if(0 != Framebuff_drawRowsBuildPlan(rowStartFb, &rowStartFb->screen, &rects[rowStartRectIdx], rowRectsCount, lines, linesSz, dstLinesUse)){
            r = -1;
        }
    }
    return r;
}

//---------------------
//-- Unplaned drawing
//---------------------
//Draws the rects in order. This means the src and dst memory area will jump.

//STPlayerDrawRectsUnplanedTask
typedef struct STPlayerDrawRectsUnplanedTask_ {
    STPlayer*               dst;    //for the mutex
    STFramebuffDrawRect*    rects;
    int                     rectsSz;
} STPlayerDrawRectsUnplanedTask;

void Player_drawRectsUnplanedTaskFunc_(void* param){
    STPlayerDrawRectsUnplanedTask* t = (STPlayerDrawRectsUnplanedTask*)param;
    //draw
    {
        int i; for(i = 0; i < t->rectsSz; i++){
            STFramebuffDrawRect* rect = &t->rects[i];
            if(rect->fb != NULL){
                STFramebuffPtr* dst = &rect->fb->screen;
                if(rect->plane == NULL){
                    //black rect
                    if(rect->fb->blackLine != NULL){
                        int yDst = rect->posCur.y;
                        int ySrc = rect->srcRectY;
                        const int bytesPerPx = (rect->fb->bitsPerPx / 8);
                        const int copyLen = bytesPerPx * rect->srcRectWidth; //ToDo: validate srcWidth <= blackLineSz
                        while(ySrc < rect->srcRectYAfterEnd){
                            unsigned char* srcLn = rect->fb->blackLine;
                            unsigned char* dstLn = &dst->ptr[(rect->fb->bytesPerLn * yDst) + (bytesPerPx * rect->posCur.x)];
                            K_ASSERT(dstLn >= dst->ptr && (dstLn + copyLen) <= (dst->ptr + dst->ptrSz)) //must be inside the destination range
                            if(copyLen > 0){
                                memcpy(dstLn, srcLn, copyLen);
                            }
                            ySrc++;
                            yDst++;
                        }
                    }
                } else {
                    //image
                    STFbRect src;
                    memset(&src, 0, sizeof(src));
                    src.x = rect->srcRectX;
                    src.y = rect->srcRectY;
                    src.width = rect->srcRectWidth;
                    src.height = (rect->srcRectYAfterEnd - rect->srcRectY);
                    if(0 != Framebuff_bitblit(rect->fb, dst, rect->posCur, rect->plane, src)){
                        K_LOG_ERROR("StreamContext, bitblit failed.\n");
                    }
                }
            }
        }
    }
    //reduce counter
    {
        pthread_mutex_lock(&t->dst->draw.mutex);
        {
            K_ASSERT(t->dst->draw.tasksPendCount > 0)
            if(t->dst->draw.tasksPendCount > 0){
                t->dst->draw.tasksPendCount--;
                if(t->dst->draw.tasksPendCount == 0){ //optimization, only awake main-thread if is the last task.
                    pthread_cond_broadcast(&t->dst->draw.cond);
                }
            }
        }
        pthread_mutex_unlock(&t->dst->draw.mutex);
    }
}

int Player_drawRectsUnplaned_(STPlayer* obj, STFramebuffDrawRect* rects, const int rectsUse){
    int r = 0;
    STPlayerDrawRectsUnplanedTask* tt = malloc(sizeof(STPlayerDrawRectsUnplanedTask) * (obj->threads.use + 1));
    if(tt != NULL){
        memset(tt, 0, sizeof(STPlayerDrawRectsUnplanedTask) * (obj->threads.use + 1));
        //
        int iRectStart = 0, ttUse = 0, rectsPerThread = (rectsUse / (obj->threads.use + 1));
        int i; for(i = 0; i < obj->threads.use; i++){
            STThread* thread = &obj->threads.arr[i];
            STPlayerDrawRectsUnplanedTask* task = &tt[ttUse];
            //
            task->dst   = obj;
            task->rects = &rects[iRectStart];
            task->rectsSz = rectsPerThread;
            if((iRectStart + task->rectsSz) > rectsUse){
                task->rectsSz = rectsUse - iRectStart;
            }
            //
            pthread_mutex_lock(&obj->draw.mutex);
            {
                obj->draw.tasksPendCount++;
            }
            pthread_mutex_unlock(&obj->draw.mutex);
            if(0 != Thread_addTask(thread, Player_drawRectsUnplanedTaskFunc_, task)){
                //K_LOG_ERROR("Thread_addTask failed.\n"); //ToDo: comment this print (risk to noisy).
                pthread_mutex_lock(&obj->draw.mutex);
                {
                    obj->draw.tasksPendCount--;
                }
                pthread_mutex_unlock(&obj->draw.mutex);
            } else {
                //next
                iRectStart += task->rectsSz;
                ttUse++;
            }
        }
        //task to draw remaining lines on this same thread
        {
            STPlayerDrawRectsUnplanedTask* task = &tt[ttUse];
            task->dst   = obj;
            task->rects = &rects[iRectStart];
            task->rectsSz = rectsUse - iRectStart;
            ttUse++;
            pthread_mutex_lock(&obj->draw.mutex);
            {
                obj->draw.tasksPendCount++;
            }
            pthread_mutex_unlock(&obj->draw.mutex);
            Player_drawRectsUnplanedTaskFunc_(task);
        }
        //wait for tasks
        {
            pthread_mutex_lock(&obj->draw.mutex);
            while(obj->draw.tasksPendCount > 0){
                K_ASSERT(obj->draw.tasksPendCount >= 0)
                pthread_cond_wait(&obj->draw.cond, &obj->draw.mutex);
            }
            pthread_mutex_unlock(&obj->draw.mutex);
        }
        free(tt);
        tt = NULL;
    }
    return r;
}

//---------------------
//-- Planed drawing
//---------------------
//Draws the lines in order. This means the dst memory area will be sequential, the src memory area will be jumping.

//STFramebuffDrawLine

//All the lines to be drawn, in dst-pointer order.
//The purpose is to organize all the memcpy instructions
//reducing the memory-jumps, to make the memcpy as closer
//as posible to as one single call for the dst-memory area.

//STPlayerDrawLinesPlanedTask

typedef struct STPlayerDrawLinesPlanedTask_ {
    STPlayer*               dst;    //for the mutex
    STFramebuffDrawLine*    lines;
    int                     linesSz;
} STPlayerDrawLinesPlanedTask;

void Player_drawLinesPlanedTaskFunc_(void* param){
    STPlayerDrawLinesPlanedTask* t = (STPlayerDrawLinesPlanedTask*)param;
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
        pthread_mutex_lock(&t->dst->draw.mutex);
        {
            K_ASSERT(t->dst->draw.tasksPendCount > 0)
            if(t->dst->draw.tasksPendCount > 0){
                t->dst->draw.tasksPendCount--;
                if(t->dst->draw.tasksPendCount == 0){ //optimization, only awake main-thread if is the last task.
                    pthread_cond_broadcast(&t->dst->draw.cond);
                }
            }
        }
        pthread_mutex_unlock(&t->dst->draw.mutex);
    }
}

int Player_drawLinesPlaned_(STPlayer* obj, STFramebuffDrawLine* lines, const int linesUse){
    int r = 0;
    STPlayerDrawLinesPlanedTask* tt = malloc(sizeof(STPlayerDrawLinesPlanedTask) * (obj->threads.use + 1));
    if(tt != NULL){
        memset(tt, 0, sizeof(STPlayerDrawLinesPlanedTask) * (obj->threads.use + 1));
        //
        int iLineStart = 0, ttUse = 0, linesPerThread = (linesUse / (obj->threads.use + 1));
        int i; for(i = 0; i < obj->threads.use; i++){
            STThread* thread = &obj->threads.arr[i];
            STPlayerDrawLinesPlanedTask* task = &tt[ttUse];
            //
            task->dst   = obj;
            task->lines = &lines[iLineStart];
            task->linesSz = linesPerThread;
            if((iLineStart + task->linesSz) > linesUse){
                task->linesSz = linesUse - iLineStart;
            }
            //
            pthread_mutex_lock(&obj->draw.mutex);
            {
                obj->draw.tasksPendCount++;
            }
            pthread_mutex_unlock(&obj->draw.mutex);
            if(0 != Thread_addTask(thread, Player_drawLinesPlanedTaskFunc_, task)){
                //K_LOG_ERROR("Thread_addTask failed.\n"); //ToDo: comment this print (risk to noisy).
                pthread_mutex_lock(&obj->draw.mutex);
                {
                    obj->draw.tasksPendCount--;
                }
                pthread_mutex_unlock(&obj->draw.mutex);
            } else {
                //next
                iLineStart += task->linesSz;
                ttUse++;
            }
        }
        //task to draw remaining lines on this same thread
        {
            STPlayerDrawLinesPlanedTask* task = &tt[ttUse];
            task->dst   = obj;
            task->lines = &lines[iLineStart];
            task->linesSz = linesUse - iLineStart;
            ttUse++;
            pthread_mutex_lock(&obj->draw.mutex);
            {
                obj->draw.tasksPendCount++;
            }
            pthread_mutex_unlock(&obj->draw.mutex);
            Player_drawLinesPlanedTaskFunc_(task);
        }
        //wait for tasks
        {
            pthread_mutex_lock(&obj->draw.mutex);
            while(obj->draw.tasksPendCount > 0){
                K_ASSERT(obj->draw.tasksPendCount >= 0)
                pthread_cond_wait(&obj->draw.cond, &obj->draw.mutex);
            }
            pthread_mutex_unlock(&obj->draw.mutex);
        }
        free(tt);
        tt = NULL;
    }
    return r;
}

int Player_draw_(STPlayer* obj){
    int r = 0, drawn = 0;
    STFramebuffDrawRect* rects = NULL;
    int rectsSz = 0, rectsUse = 0;
    //
    struct timeval start;
    gettimeofday(&start, NULL);
    //precalculate drawing rects
    if(0 == Player_drawGetRects_(obj, rects, rectsSz, &rectsUse)){
        K_LOG_VERBOSE("Player, drawing nothing-to-draw.\n");
    } else {
        //resize rects array
        if(rectsUse > 0){
            rects = malloc(sizeof(STFramebuffDrawRect) * rectsUse);
            if(rects == NULL){
                K_LOG_INFO("Player, draw, recs[%d] culd not be allocated.\n", rectsUse);
                r = -1;
            } else {
                rectsSz = rectsUse;
                rectsUse = 0;
                if(0 != Player_drawGetRects_(obj, rects, rectsSz, &rectsUse)){
                    K_LOG_INFO("Player, draw, failed recs[%d].\n", rectsSz);
                } else {
                    //-------
                    //draw rects unplaned (unoptimized)
                    //-------
                    switch(obj->cfg.drawMode){
                        case ENPlayerDrawMode_Src:
                            //-------
                            //draw rects unplaned
                            //-------
                            if(0 != Player_drawRectsUnplaned_(obj, rects, rectsUse)){
                                K_LOG_INFO("Player, draw, failed draw rects[%d].\n", rectsUse);
                            } else {
                                K_LOG_VERBOSE("Player, drawn rects[%d].\n", rectsSz);
                                drawn = 1;
                            }
                            break;
                        default: //ENPlayerDrawMode_Dst
                            //-------
                            //draw lines planed (optimized)
                            //-------
                            {
                                int linesSz = 0;
                                //count-lines
                                int i; for(i = 0 ; i < rectsUse; i++){
                                    STFramebuffDrawRect* rect = &rects[i];
                                    if(rect->srcRectY < rect->srcRectYAfterEnd){
                                        linesSz += (rect->srcRectYAfterEnd - rect->srcRectY);
                                    }
                                }
                                if(linesSz > 0){
                                    STFramebuffDrawLine* lines = (STFramebuffDrawLine*)malloc(sizeof(STFramebuffDrawLine) * linesSz);
                                    if(lines == NULL){
                                        K_LOG_INFO("Player, draw, lines[%d] culd not be allocated.\n", linesSz);
                                        r = -1;
                                    } else {
                                        int linesUse = 0;
                                        memset(lines, 0, sizeof(STFramebuffDrawLine) * linesSz);
                                        if(0 != Player_drawGetLines_(obj, rects, rectsUse, lines, linesSz, &linesUse)){
                                            K_LOG_INFO("Player, draw, failed get lines[%d / %d].\n", linesUse, linesSz);
                                        } else if(0 != Player_drawLinesPlaned_(obj, lines, linesUse)){
                                            K_LOG_INFO("Player, draw, failed draw lines[%d / %d].\n", linesUse, linesSz);
                                        } else {
                                            K_LOG_VERBOSE("Player, drawn rects[%d] lines[%d].\n", rectsSz, linesSz);
                                            drawn = 1;
                                        }
                                        //
                                        free(lines);
                                        lines = NULL;
                                    }
                                }
                            }
                            break;
                    }
                }
            }
        }
    }
    //add stats
    if(r == 0 && drawn){
        struct timeval end; long ms;
        gettimeofday(&end, NULL);
        ms = msBetweenTimevals(&start, &end);
        if(ms >= 0){
            pthread_mutex_lock(&obj->stats.mutex);
            {
                if(obj->stats.curSec.draw.count == 0){
                    obj->stats.curSec.draw.msMin = ms;
                    obj->stats.curSec.draw.msMax = ms;
                } else {
                    if(obj->stats.curSec.draw.msMin > ms) obj->stats.curSec.draw.msMin = ms;
                    if(obj->stats.curSec.draw.msMax < ms) obj->stats.curSec.draw.msMax = ms;
                }
                obj->stats.curSec.draw.msSum += ms;
                obj->stats.curSec.draw.count++;
            }
            pthread_mutex_unlock(&obj->stats.mutex);
        }
    }
    //release
    {
        if(rects != NULL){
            free(rects);
            rects = NULL;
        }
        rectsSz = rectsUse = 0;
    }
    return r;
}

//-------------------
//-- StreamContext --
//-------------------

void StreamContext_init(STStreamContext* ctx){
    memset(ctx, 0, sizeof(STStreamContext));
    //buff
    {
        ctx->buff.buffUse   = 0;
        ctx->buff.buffSz    = (1024 * 64);
        ctx->buff.buff      = (unsigned char*)malloc(ctx->buff.buffSz);
    }
    //file
    {
        ctx->file.fd = -1;
    }
    //net
    {
        //
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
    //buff
    {
        if(ctx->buff.buff != NULL){
            free(ctx->buff.buff);
            ctx->buff.buff = NULL;
        }
        ctx->buff.buffSz = 0;
    }
    //file
    {
        if(ctx->file.fd >= 0){
            close(ctx->file.fd);
            ctx->file.fd = -1;
        }
    }
    //net
    {
        if(ctx->net.hostResolver != NULL){
            if(0 != gai_cancel(ctx->net.hostResolver)){
                //error
            }
            //ToDo: how to cleanup?
            //if(ctx->net.hostResolver->ar_result != NULL){
            //    freeaddrinfo(ctx->net.hostResolver->ar_result);
            //}
            free(ctx->net.hostResolver);
            ctx->net.hostResolver = NULL;
        }
        if(ctx->net.socket){
            close(ctx->net.socket);
            ctx->net.socket = 0;
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
        if(ctx->dec.fd >= 0){
            if(0 != StreamContext_stopAndCleanupBuffs(ctx, &ctx->dec.dst, ctx->dec.fd)){
                K_LOG_WARN("StreamContext_stopAndCleanupBuffs(dst) failed.\n");
            }
            if(0 != StreamContext_stopAndCleanupBuffs(ctx, &ctx->dec.src, ctx->dec.fd)){
                K_LOG_WARN("StreamContext_stopAndCleanupBuffs(src) failed.\n");
            }
            if(0 != StreamContext_eventsUnsubscribe(ctx, ctx->dec.fd)){
                K_LOG_ERROR("StreamContext, unsubscribe failed.\n");
            }
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
    if(rr2 == 0){
        if(dstValue != NULL){
            *dstValue = ctrl.value;
        }
    } else {
        const STErrCode* err = _getErrCode(errno);
        if(err == NULL){
            K_LOG_ERROR("VIDIOC_G_CTRL, V4L2_CID_MIN_BUFFERS_FOR_CAPTURE, returned errno(%d).\n", errno);
        } else {
            K_LOG_ERROR("VIDIOC_G_CTRL, V4L2_CID_MIN_BUFFERS_FOR_CAPTURE, returned '%s'.\n", err->str);
        }
    }
    return rr2;
}

int StreamContext_initAndPrepareSrc(STStreamContext* ctx, int fd, const int buffersAmmount, const int print){
    int r = -1;
    //should be stopped
    if(0 != Buffers_allocBuffs(&ctx->dec.src, fd, buffersAmmount, print)){
        K_LOG_ERROR("StreamContext, Buffers_allocBuffs(%d) failed.\n", buffersAmmount);
    } else if(ctx->dec.src.sz <= 0){
        K_LOG_ERROR("StreamContext, Buffers_allocBuffs(%d) created zero buffs.\n", buffersAmmount);
    //} else if(0 != Buffers_export(&ctx->dec.src, fd)){
    //    K_LOG_ERROR("StreamContext, Buffers_export(%d) failed.\n", ctx->dec.src.sz);
    } else if(0 != Buffers_mmap(&ctx->dec.src, fd)){
        K_LOG_ERROR("StreamContext, Buffers_mmap(%d) failed.\n", ctx->dec.src.sz);
    } else {
        //if(ctx->dec.src.sz == buffersAmmount){
        //    K_LOG_INFO("StreamContext, inited device with %d buffers.\n", buffersAmmount);
        //} else {
        //    K_LOG_INFO("StreamContext, inited device with %d of %d buffers.\n", ctx->dec.src.sz, buffersAmmount);
        //}
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
    if(0 != Buffers_setFmt(&ctx->dec.dst, ctx->dec.fd, ctx->cfg.dstPixFmt, 1, 0, 1 /*getCompositionRect*/, (printDstFmt != NULL ? 1 : 0))){
        K_LOG_ERROR("Buffers_setFmt(dst) failed: '%s'.\n", ctx->cfg.device);
    } else if(0 != StreamContext_getMinBuffersForDst(ctx, &ctx->dec.dst.enqueuedRequiredMin)){
        K_LOG_ERROR("StreamContext_getMinBuffersForDst(dst) failed: '%s'.\n", ctx->cfg.device);
    } else if(ctx->dec.dst.enqueuedRequiredMin <= 0){
        K_LOG_ERROR("StreamContext_getMinBuffersForDst(dst) returned(%d): '%s'.\n", ctx->dec.dst.enqueuedRequiredMin, ctx->cfg.device);
    } else if(0 != Buffers_allocBuffs(&ctx->dec.dst, ctx->dec.fd, ctx->dec.dst.enqueuedRequiredMin /*+ 1*/, (printDstFmt != NULL ? 1 : 0))){ //+1 to keep a copy for rendering
        K_LOG_ERROR("Buffers_allocBuffs(%d, dst) failed: '%s'.\n", (ctx->dec.dst.enqueuedRequiredMin /*+ 1*/), ctx->cfg.device);
    } else if(ctx->dec.dst.sz <= 0){
        K_LOG_ERROR("Buffers_allocBuffs(%d, dst) created zero buffers: '%s'.\n", ctx->dec.dst.sz, ctx->cfg.device);
    } else if(ctx->dec.dst.sz < ctx->dec.dst.enqueuedRequiredMin){
        K_LOG_ERROR("Buffers_allocBuffs(%d, dst) created below minimun(%d) buffers: '%s'.\n", ctx->dec.dst.sz, ctx->dec.dst.enqueuedRequiredMin, ctx->cfg.device);
    //} else if(0 != Buffers_export(&ctx->dec.dst, ctx->dec.fd)){
    //    K_LOG_ERROR("Buffers_export(%d, dst) failed: '%s'.\n", ctx->dec.dst.sz, ctx->cfg.device);
    } else if(0 != Buffers_mmap(&ctx->dec.dst, ctx->dec.fd)){
        K_LOG_ERROR("Buffers_mmap(%d, dst) failed: '%s'.\n", ctx->dec.dst.sz, ctx->cfg.device);
    } else if(0 != Buffers_enqueueMinimun(&ctx->dec.dst, ctx->dec.fd, ctx->dec.dst.enqueuedRequiredMin)){
        K_LOG_ERROR("Buffers_enqueueMinimun(%d / %d, dst) failed: '%s'.\n", ctx->dec.dst.enqueuedRequiredMin, ctx->dec.dst.sz, ctx->cfg.device);
    } else if(0 != Buffers_start(&ctx->dec.dst, ctx->dec.fd)){
        K_LOG_ERROR("Buffers_start(%d, dst) failed: '%s'.\n", ctx->dec.dst.sz, ctx->cfg.device);
    } else {
        if(ctx->dec.dst.sz != (ctx->dec.dst.enqueuedRequiredMin /*+ 1*/)){
            K_LOG_INFO("StreamContext, dst-started (%d/%d buffers): '%s'.\n", ctx->dec.dst.sz, (ctx->dec.dst.enqueuedRequiredMin /*+ 1*/), ctx->cfg.device);
        } else {
            K_LOG_VERBOSE("StreamContext, dst-started (%d buffers): '%s'.\n", (ctx->dec.dst.enqueuedRequiredMin /*+ 1*/), ctx->cfg.device);
        }
        //if(ctx->dec.dst.sz == ctx->dec.dst.enqueuedRequiredMin){
        //    K_LOG_WARN("attempt to allocate one extra buffer failed, this implies an extra memcopy() per decoded-frame: '%s'.\n", ctx->cfg.device);
        //}
        r = 0;
        //
        ctx->drawPlan.lastPixelformat = ctx->dec.dst.pixelformat;
        if(ctx->dec.dst.composition.width > 0 && ctx->dec.dst.composition.height > 0){
            ctx->drawPlan.lastCompRect = ctx->dec.dst.composition;
        }
        //update poll (dst started)
        StreamContext_updatePollMask_(ctx, plyr);
        //reorganize
        if(0 != Player_organize(plyr)){
            K_LOG_ERROR("Player_organize failed after dst-resized.\n");
        }
    }
    return r;
}

int StreamContext_stopAndCleanupBuffs(STStreamContext* ctx, STBuffers* buffs, int fd){
    int r = -1;
    //IMPORTANT NOTE: if device exposed 'V4L2_BUF_CAP_SUPPORTS_ORPHANED_BUFS' capability
    //, the buffer is not released untill is unmaped and closed.
    //
    if(fd >= 0){
        if(0 != Buffers_stop(buffs, fd)){
            K_LOG_ERROR("Buffers_stop(dst) failed: '%s'.\n", ctx->cfg.device);
        }
        //
        if(0 != Buffers_allocBuffs(buffs, fd, 0, 0)){
            K_LOG_ERROR("Buffers_allocBuffs(dst, 0) failed: '%s'.\n", ctx->cfg.device);
        } else {
            r = 0;
        }
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
            K_LOG_ERROR("StreamContext, Subscription to event V4L2_EVENT_SOURCE_CHANGE errno(%d).\n", errno);
            return -1;
        } else {
            K_LOG_VERBOSE("StreamContext, Subscription to event V4L2_EVENT_SOURCE_CHANGE success.\n");
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
            K_LOG_ERROR("StreamContext, Subscription to event V4L2_EVENT_EOS errno(%d).\n", errno);
            return -1;
        } else {
            K_LOG_VERBOSE("StreamContext, Subscription to event V4L2_EVENT_EOS success.\n");
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
        K_LOG_ERROR("StreamContext, Unscubscribe failed errno(%d).\n", errno);
        return -1;
    } else {
        K_LOG_VERBOSE("StreamContext, Unscubscribe success.\n");
    }
    return 0;
}

void StreamContext_tick(STStreamContext* ctx, struct STPlayer_* plyr, unsigned int ms){
    //decoder
    if(ctx->dec.fd < 0){
        ctx->dec.msOpen = 0;
        ctx->dec.msFirstFrameFed = 0;
        ctx->dec.msFirstFrameOut = 0;
        ctx->dec.framesInSinceOpen = 0;
        ctx->dec.framesOutSinceOpen = 0;
        //
        if(ctx->dec.msToReopen <= ms){
            ctx->dec.msToReopen = 0;
        } else {
            ctx->dec.msToReopen -= ms;
            //if(ctx->dec.shouldBeOpen && (ctx->dec.msToReopen / 1000) != ((ctx->dec.msToReopen + ms) / 1000)){
            //    K_LOG_INFO("StreamContext_tick, waiting %d secs to reopen attempt: %s.\n", (ctx->dec.msToReopen / 1000), ctx->cfg.path);
            //}
        }
        if(ctx->dec.shouldBeOpen){
            //reopen
            if(ctx->dec.msToReopen == 0){
                //ToDo: execute this action once no src-buffer is queued or timedout src-bufer flush?
                if(0 != StreamContext_open(ctx, plyr, ctx->cfg.device, ctx->cfg.server, ctx->cfg.port, ctx->cfg.keepAlive, ctx->cfg.path, ctx->cfg.srcPixFmt, ctx->cfg.buffersAmmount, ctx->cfg.planesPerBuffer, ctx->cfg.sizePerPlane, ctx->cfg.dstPixFmt, ctx->cfg.connTimeoutSecs, ctx->cfg.decoderTimeoutSecs, ctx->cfg.framesSkip, ctx->cfg.framesFeedMax)){
                    K_LOG_ERROR("StreamContext, streamAdd failed: '%s' @ '%s'.\n", ctx->cfg.path, ctx->cfg.device);
                } else {
                    K_LOG_VERBOSE("StreamContext (%lld), tick device reopened and added to poll: '%s'.\n", (long long)ctx, ctx->cfg.path);
                    //trigger network connection (if not keepAlive)
                    if(!ctx->cfg.keepAlive){
                        ctx->net.msToReconnect = 0;
                    }
                }
                ctx->dec.msToReopen = (plyr->cfg.decoderWaitRecopenSecs <= 0 ? 1 : plyr->cfg.decoderWaitRecopenSecs) * 1000;
            }
        }
    }
    //file
    if(ctx->file.fd > 0){
        int simConnTimeout = 0;
        //
        if(plyr->cfg.dbg.simNetworkTimeout > 0){
            if((rand() % plyr->cfg.dbg.simNetworkTimeout) == 0){
                K_LOG_WARN("StreamContext_tick, forcing/simulating a NETWORK timeout (1 / %d prob.): '%s'.\n", plyr->cfg.dbg.simNetworkTimeout, ctx->cfg.path);
                simConnTimeout = 1;
            }
        }
        //connecting or connected
        ctx->file.msWithoutRead += ms;
        //continue reading (after screen refresh)
        if(ctx->file.fd > 0 && ctx->buff.screenRefreshSeqBlocking > 0 && ctx->buff.screenRefreshSeqBlocking != plyr->anim.tickSeq){
            ctx->buff.screenRefreshSeqBlocking = 0;
            StreamContext_updatePollMaskFile_(ctx, plyr);
        }
        //timeout
        if(ctx->file.fd > 0 && (simConnTimeout || (ctx->cfg.connTimeoutSecs > 0 && ctx->file.msWithoutRead > (ctx->cfg.connTimeoutSecs * 1000)))){
            if(simConnTimeout){
                K_LOG_ERROR("StreamContext_tick, net, simulated-connection-timeout('%s:%d') after %ds not reading: '%s'.\n", ctx->cfg.server, ctx->cfg.port, (ctx->file.msWithoutRead / 1000), ctx->cfg.path);
            } else {
                //the lastest inactive trigger was reading
                K_LOG_ERROR("StreamContext_tick, net, connection-timeout('%s:%d') after %ds not reading: '%s'.\n", ctx->cfg.server, ctx->cfg.port, (ctx->file.msWithoutRead / 1000), ctx->cfg.path);
            }
            if(ctx->file.fd > 0){
                const char* reason = "file timeout";
                if(0 != StreamContext_shutdownStartByFileClosed_(ctx, plyr, reason)){
                    K_LOG_ERROR("StreamContext_tick, StreamContext_shutdownStart failed (at '%s').\n", reason);
                }
            }
        }
    } else if((ctx->cfg.server == NULL || ctx->cfg.server[0] == '\0') && ctx->cfg.port <= 0 && ctx->cfg.path != NULL && ctx->cfg.path[0] != '\0'){
        //waiting
        if(ctx->file.msToReconnect <= ms){
            ctx->file.msToReconnect = 0;
        } else {
            ctx->file.msToReconnect -= ms;
            //if((ctx->file.msToReconnect / 1000) != ((ctx->file.msToReconnect + ms) / 1000)){
            //    K_LOG_INFO("StreamContext_tick, waiting %d secs to reconnect attempt: '%s'.\n", (ctx->file.msToReconnect / 1000), ctx->cfg.path);
            //}
        }
        //reconnect
        if(ctx->file.msToReconnect == 0){
            ctx->file.msToReconnect = (plyr->cfg.connWaitReconnSecs > 0 ? plyr->cfg.connWaitReconnSecs : 1) * 1000;
            //
            if(!(ctx->shuttingDown.isActive && ctx->shuttingDown.isPermanent)){
                int fd = open(ctx->cfg.path, O_RDONLY | O_NONBLOCK);
                if(fd < 0){
                    K_LOG_ERROR("StreamContext, could not open file: '%s'.\n", ctx->cfg.path);
                } else {
                    K_LOG_INFO("StreamContext, file opened: '%s'.\n", ctx->cfg.path);
                    //add to pollster
                    if(0 != Player_pollAdd(plyr, ENPlayerPollFdType_SrcFile, StreamContext_pollCallback, ctx, fd, POLLIN)){ //read
                        K_LOG_ERROR("poll-add-failed to '%s:%d': '%s'.\n", ctx->cfg.server, ctx->cfg.port, ctx->cfg.path);
                    } else {
                        K_LOG_INFO("StreamContext, socket added to poll: '%s:%d': '%s'.\n", ctx->cfg.server, ctx->cfg.port, ctx->cfg.path);
                        ctx->file.fd = fd; fd = -1; //consume
                        ctx->file.msWithoutRead = 0;
                        //reaet vars
                        ctx->buff.buffUse = 0;
                        ctx->buff.buffCsmd = 0;
                        //
                        ctx->buff.nal.zeroesSeqAccum    = 0;   //reading posible headers '0x00 0x00 0x00 0x01' (start of a NAL)
                        //ctx->file.resp.nal.startsCount = 0;    //total NALs found
                        //
                        ctx->frames.fillingNalSz = 0;
                        if(ctx->frames.filling != NULL){
                            //add for future pull (reuse)
                            if(0 != VideoFrames_pushFrameOwning(&ctx->frames.reusable, ctx->frames.filling)){
                                K_LOG_INFO("StreamContext_tick, VideoFrames_pushFrameOwning failed: '%s'.\n", ctx->cfg.path);
                                VideoFrame_release(ctx->frames.filling);
                                free(ctx->frames.filling);
                            }
                            ctx->frames.filling = NULL;
                        }
                    }
                    //release (if not consumed)
                    if(fd >= 0){
                        close(fd);
                        fd = -1;
                    }
                }
            }
        }
    }
    //net
    if(ctx->net.hostResolver != NULL || ctx->net.socket > 0){
        ctx->net.msSinceStart += ms;
    }
    //net
    if(ctx->net.socket > 0){
        int closeConnn = 0, simConnTimeout = 0;
        //
        if(plyr->cfg.dbg.simNetworkTimeout > 0){
            if((rand() % plyr->cfg.dbg.simNetworkTimeout) == 0){
                K_LOG_WARN("StreamContext_tick, forcing/simulating a NETWORK timeout (1 / %d prob.): '%s'.\n", plyr->cfg.dbg.simNetworkTimeout, ctx->cfg.path);
                simConnTimeout = 1;
            }
        }
        //connecting or connected
        ctx->net.msWithoutSend += ms;
        ctx->net.msWithoutRecv += ms;
        //
        if(simConnTimeout){
            K_LOG_ERROR("StreamContext_tick, net, simulated-connection-timeout('%s:%d') after %ds not writting and %ds not reading: '%s'.\n", ctx->cfg.server, ctx->cfg.port, (ctx->net.msWithoutSend / 1000), (ctx->net.msWithoutRecv / 1000), ctx->cfg.path);
            closeConnn = 1;
        } else if((ctx->cfg.connTimeoutSecs > 0 && ctx->net.msWithoutSend > (ctx->cfg.connTimeoutSecs * 1000) && ctx->net.msWithoutRecv > (ctx->cfg.connTimeoutSecs * 1000))){
            if(ctx->net.msWithoutSend == ctx->net.msWithoutRecv){
                //the lastest inactive trigger was both (connection attemp)
                K_LOG_ERROR("StreamContext_tick, net, connection-timeout('%s:%d') after %ds: '%s'.\n", ctx->cfg.server, ctx->cfg.port, (ctx->net.msWithoutSend / 1000), ctx->cfg.path);
            } else if(ctx->net.msWithoutSend < ctx->net.msWithoutRecv){
                //the lastest inactive trigger was writting
                K_LOG_ERROR("StreamContext_tick, net, connection-timeout('%s:%d') after %ds not writting: '%s'.\n", ctx->cfg.server, ctx->cfg.port, (ctx->net.msWithoutSend / 1000), ctx->cfg.path);
            } else {
                //the lastest inactive trigger was reading
                K_LOG_ERROR("StreamContext_tick, net, connection-timeout('%s:%d') after %ds not reading: '%s'.\n", ctx->cfg.server, ctx->cfg.port, (ctx->net.msWithoutRecv / 1000), ctx->cfg.path);
            }
            closeConnn = 1;
        } else if(!ctx->cfg.keepAlive && !ctx->dec.shouldBeOpen){
            K_LOG_VERBOSE("StreamContext_tick, net, closing conn out-of-screen: '%s:%d%s'.\n", ctx->cfg.server, ctx->cfg.port, ctx->cfg.path);
            closeConnn = 1;
        }
        if(closeConnn && ctx->net.socket > 0){
            Player_pollAutoRemove(plyr, ENPlayerPollFdType_SrcSocket, ctx, ctx->net.socket);
            close(ctx->net.socket);
            ctx->net.socket = 0;
            ctx->net.msToReconnect = (plyr->cfg.connWaitReconnSecs > 0 ? plyr->cfg.connWaitReconnSecs : 1) * 1000;
        }
    } else if(ctx->net.hostResolver == NULL && ctx->cfg.server != NULL && ctx->cfg.server[0] != '\0' && ctx->cfg.port > 0 && ctx->cfg.path != NULL && ctx->cfg.path[0] != '\0'){
        //waiting
        if(ctx->net.msToReconnect <= ms){
            ctx->net.msToReconnect = 0;
        } else {
            ctx->net.msToReconnect -= ms;
            //if((ctx->net.msToReconnect / 1000) != ((ctx->net.msToReconnect + ms) / 1000)){
            //    K_LOG_INFO("StreamContext_tick, waiting %d secs to reconnect attempt: '%s'.\n", (ctx->net.msToReconnect / 1000), ctx->cfg.path);
            //}
        }
        //reconnect
        if(ctx->net.msToReconnect == 0 && (ctx->cfg.keepAlive || ctx->dec.shouldBeOpen)){
            ctx->net.msToReconnect = (plyr->cfg.connWaitReconnSecs > 0 ? plyr->cfg.connWaitReconnSecs : 1) * 1000;
            //
            if(!(ctx->shuttingDown.isActive && ctx->shuttingDown.isPermanent)){
                struct gaicb* hostResolver = (struct gaicb*)malloc(sizeof(struct gaicb));
                memset(hostResolver, 0, sizeof(*hostResolver));
                hostResolver->ar_name = ctx->cfg.server;
                if(0 != getaddrinfo_a(GAI_NOWAIT, &hostResolver, 1, NULL)){
                    K_LOG_ERROR("StreamContext_tick, net, getaddrinfo_a failed (start): '%s' / '%s'.\n", ctx->cfg.server, ctx->cfg.path);
                } else {
                    //release previous
                    {
                        if(ctx->net.hostResolver != NULL){
                            if(0 != gai_cancel(ctx->net.hostResolver)){
                                //error
                            }
                            //ToDo: how to cleanup?
                            //if(ctx->net.hostResolver->ar_result != NULL){
                            //    freeaddrinfo(ctx->net.hostResolver->ar_result);
                            //}
                            free(ctx->net.hostResolver);
                            ctx->net.hostResolver = NULL;
                        }
                    }
                    //set new
                    ctx->net.hostResolver = hostResolver;
                    hostResolver = NULL; //consume
                    //reset stat
                    ctx->net.msSinceStart = 0;   //connection start (resolve or socket)
                    ctx->net.msToResolve = 0;    //time since start to resolve host
                    ctx->net.msToConnect = 0;    //time since start to send request
                    ctx->net.msToRespStart = 0;  //time since start to receive response header
                    ctx->net.msToRespHead = 0;   //time since complete to receive response header
                    ctx->net.msToRespBody = 0;   //time since start to receive response body
                    ctx->net.msToFirstUnit = 0;  //time since complete to receive first unit
                    ctx->net.bytesSent      = 0;
                    ctx->net.bytesRcvd      = 0;
                    ctx->net.unitsRcvd      = 0;
                }
                //release (if not consumed)
                if(hostResolver != NULL){
                    free(hostResolver);
                    hostResolver = NULL;
                }
            }
        }
    }
    //net (resolver)
    if(ctx->net.hostResolver != NULL){
        int rslvRelease = 0;
        int reslvRet = gai_error(ctx->net.hostResolver);
        if(reslvRet == EAI_INPROGRESS){
            //active
        } else if(reslvRet != 0){
            //error
            K_LOG_ERROR("StreamContext_tick, net, getaddrinfo_a failed (progress): '%s' / '%s'.\n", ctx->cfg.server, ctx->cfg.path);
            rslvRelease = 1;
        } else {
            //success
            struct addrinfo* res = ctx->net.hostResolver->ar_result;
            struct in_addr hostAddr; int hostAddrFnd = 0;
            memset(&hostAddr, 0, sizeof(struct in_addr));
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
            if(!hostAddrFnd){
                K_LOG_ERROR("StreamContext_tick, net, host-addr-not-found('%s'): '%s'.\n", ctx->cfg.server, ctx->cfg.path);
            } else {
                //connect
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
                    K_LOG_ERROR("StreamContext_tick, net, socket creation failed: '%s'.\n", ctx->cfg.path);
                    ctx->net.socket = 0;
                }
                //config
#               ifdef SO_NOSIGPIPE
                if(sckt && sckt != INVALID_SOCKET){
                    int v = 1; //(noSIGPIPE ? 1 : 0);
                    if(setsockopt(sckt, SOL_SOCKET, SO_NOSIGPIPE, (const char*)&v, sizeof(v)) < 0){
                        K_LOG_ERROR("StreamContext_tick, net, socket SO_NOSIGPIPE option failed: '%s'.\n", ctx->cfg.path);
                        close(sckt);
                        sckt = 0;
                    }
                }
#               endif
                //non-block
                if(sckt && sckt != INVALID_SOCKET){
                    int flags, nonBlocking = 1;
                    if ((flags = fcntl(sckt, F_GETFL, 0)) == -1){
                        K_LOG_ERROR("StreamContext_tick, net, F_GETFL failed: '%s'.\n", ctx->cfg.path);
                        close(sckt);
                        sckt = 0;
                    } else {
#                       ifdef O_NONBLOCK
                        if(nonBlocking) flags |= O_NONBLOCK;
                        else flags &= ~O_NONBLOCK;
#                       endif
#                       ifdef O_NDELAY
                        if(nonBlocking) flags |= O_NDELAY;
                        else flags &= ~O_NDELAY;
#                       endif
#                       ifdef FNDELAY
                        if(nonBlocking) flags |= FNDELAY;
                        else flags &= ~FNDELAY;
#                       endif
                        if(fcntl(sckt, F_SETFL, flags) == -1){
                            K_LOG_ERROR("StreamContext_tick, net, F_SETFL O_NONBLOCK option failed: '%s'.\n", ctx->cfg.path);
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
                                K_LOG_ERROR("StreamContext_tick, connect-start-failed to '%s:%d': '%s'.\n", ctx->cfg.server, ctx->cfg.port, ctx->cfg.path);
                                close(sckt);
                                sckt = 0;
                            } else {
                                K_LOG_VERBOSE("StreamContext_tick, net, connect-started to '%s:%d': '%s'.\n", ctx->cfg.server, ctx->cfg.port, ctx->cfg.path);
                            }
                        }
                    }
                    //add to pollster
                    if(0 != Player_pollAdd(plyr, ENPlayerPollFdType_SrcSocket, StreamContext_pollCallback, ctx, sckt, POLLOUT)){ //write
                        K_LOG_ERROR("StreamContext_tick, poll-add-failed to '%s:%d': '%s'.\n", ctx->cfg.server, ctx->cfg.port, ctx->cfg.path);
                        close(sckt);
                        sckt = 0;
                    } else {
                        K_LOG_VERBOSE("StreamContext_tick, socket added to poll: '%s:%d': '%s'.\n", ctx->cfg.server, ctx->cfg.port, ctx->cfg.path);
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
                        K_LOG_VERBOSE("StreamContext_tick, net http-req built (reused %d/%d buffer): '%s'.\n", ctx->net.req.payUse, ctx->net.req.paySz, ctx->cfg.path);
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
                        K_LOG_VERBOSE("StreamContext_tick, net http-req built (new %d/%d buffer): '%s'.\n", ctx->net.req.payUse, ctx->net.req.paySz, ctx->cfg.path);
                    }
                }
                //reset vars
                {
                    ctx->net.resp.headerEndSeq = 0;
                    ctx->net.resp.headerSz = 0;
                    ctx->net.resp.headerEnded = 0;
                    //
                    ctx->buff.buffUse = 0;
                    ctx->buff.buffCsmd = 0;
                    //
                    ctx->buff.nal.zeroesSeqAccum    = 0;   //reading posible headers '0x00 0x00 0x00 0x01' (start of a NAL)
                    //ctx->buff.nal.startsCount = 0;    //total NALs found
                    //
                    ctx->frames.fillingNalSz = 0;
                    if(ctx->frames.filling != NULL){
                        //add for future pull (reuse)
                        if(0 != VideoFrames_pushFrameOwning(&ctx->frames.reusable, ctx->frames.filling)){
                            K_LOG_INFO("StreamContext_tick, VideoFrames_pushFrameOwning failed: '%s'.\n", ctx->cfg.path);
                            VideoFrame_release(ctx->frames.filling);
                            free(ctx->frames.filling);
                        }
                        ctx->frames.filling = NULL;
                    }
                }
                ctx->net.msToResolve = ctx->net.msSinceStart;
                if(ctx->net.msToResolve > 1000){
                    K_LOG_INFO("StreamContext_tick, %s ms to resolve-host: '%s'.\n", ctx->net.msToResolve, ctx->cfg.path);
                } else {
                    K_LOG_VERBOSE("StreamContext_tick, %s ms to resolve-host: '%s'.\n", ctx->net.msToResolve, ctx->cfg.path);
                }
            }
            rslvRelease = 1;
        }
        //release
        if(rslvRelease){
            if(0 != gai_cancel(ctx->net.hostResolver)){
                //error
            }
            //ToDo: how to cleanup?
            //if(ctx->net.hostResolver->ar_result != NULL){
            //    freeaddrinfo(ctx->net.hostResolver->ar_result);
            //}
            free(ctx->net.hostResolver);
            ctx->net.hostResolver = NULL;
        }
    }
    //buffs
    if(ctx->dec.fd >= 0){
        if(!ctx->dec.shouldBeOpen){
            if(!ctx->shuttingDown.isActive){
                const int isPermanent = 0;
                if(0 != StreamContext_shutdownStart(ctx, plyr, isPermanent)){
                    K_LOG_ERROR("StreamContext_tick, StreamContext_shutdownStart failed (at tick).\n");
                }
            }
        } else {
            int simDecoderTimeout = 0;
            if(plyr->cfg.dbg.simDecoderTimeout > 0){
                if((rand() % plyr->cfg.dbg.simDecoderTimeout) == 0){
                    K_LOG_WARN("StreamContext_tick, forcing/simulating a DECODER timeout (1 / %d prob.): '%s'.\n", plyr->cfg.dbg.simDecoderTimeout, ctx->cfg.path);
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
                        K_LOG_ERROR("StreamContext_tick, simulated-decoder timeout: %u ms inactive while ON and frames arriving: '%s'.\n", msDecoderInative, ctx->cfg.path);
                    } else {
                        K_LOG_ERROR("StreamContext_tick, decoder timeout: %u ms inactive while ON and frames arriving: '%s'.\n", msDecoderInative, ctx->cfg.path);
                    }
                    {
                        //IMPORTANT: in raspberry pi-4 'seek' is aparently unsupported,
                        //           leaking buffers and producing messages
                        //           "bcm2835_codec_flush_buffers: Timeout waiting for buffers to be returned".
                        //           Instead of stopping and resuming the src-buffers, is safer to reopen the device file.
                        //
                        //shutdown
                        if(!ctx->shuttingDown.isActive){
                            const int isPermanent = 0;
                            if(0 != StreamContext_shutdownStart(ctx, plyr, isPermanent)){
                                K_LOG_ERROR("StreamContext_tick, StreamContext_shutdownStart failed (at decoder timeout).\n");
                            }
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
    }
    //flushing tick
    if(ctx->flushing.isActive && !ctx->flushing.isCompleted){
        if(0 != StreamContext_flushTick(ctx, ms, "StreamContext_tick")){
            K_LOG_WARN("StreamContext_tick, StreamContext_flushTick failed: '%s'.\n", ctx->cfg.path);
        }
    }
    //shutdown tick
    if(ctx->shuttingDown.isActive && !ctx->shuttingDown.isCompleted){
        if(0 != StreamContext_shutdownTick(ctx, plyr, ms, "StreamContext_tick")){
            K_LOG_WARN("StreamContext_tick, StreamContext_shutdownTick failed: '%s'.\n", ctx->cfg.path);
        }
    }
    //
    if(ctx->dec.fd >= 0){
        ctx->dec.msOpen += ms;
    }
    ctx->dec.msWithoutFeedFrame += ms;
}

int StreamContext_flushStart(STStreamContext* ctx){
    int r = -1;
    if(!ctx->flushing.isActive){
        r = 0;
        memset(&ctx->flushing, 0, sizeof(ctx->flushing));
        ctx->flushing.isActive = 1;
        if(ctx->dec.fd < 0){
            //K_LOG_INFO("StreamContext, flushStart  starting and completed (no decoder open).\n");
            ctx->flushing.isSrcDone = 1;
            ctx->flushing.isCompleted = 1;
        } else {
            //K_LOG_INFO("StreamContext, flushStart  starting.\n");
            //flush-tick once to porgress now if posible
            if(0 != StreamContext_flushTick(ctx, 0, "StreamContext_flushStart")){
                K_LOG_ERROR("StreamContext_flushStart, StreamContext_flushTick failed.\n");
                r = -1;
            }
        }
    }
    return r;
}

int StreamContext_flushTick(STStreamContext* ctx, const int ms, const char* srcLocation){
    int r = -1;
    if(ctx->flushing.isActive && !ctx->flushing.isCompleted){
        r = 0;
        ctx->flushing.msAccum += ms;
        //
        if(ctx->dec.fd < 0){
            if(!ctx->flushing.isSrcDone || !ctx->flushing.isCompleted){
                K_LOG_WARN("StreamContext, device closed before flushing (%ums) (at '%s').\n", ctx->flushing.msAccum, srcLocation);
                ctx->flushing.isSrcDone = 1;
                ctx->flushing.isCompleted = 1;
            }
        } else {
            //src
            if(!ctx->flushing.isSrcDone){
                //try to dequeue bufer
                while(ctx->dec.src.enqueuedCount > 0){
                    STBuffer* buff = NULL;
                    if(0 != Buffers_dequeue(&ctx->dec.src, ctx->dec.fd, &buff, NULL)){
                        buff = NULL;
                        break;
                    }
                }
                if(ctx->dec.src.enqueuedCount == 0){
                    //K_LOG_INFO("StreamContext, flushing src-completed (%ums) (at '%s').\n", ctx->flushing.msAccum, srcLocation);
                    ctx->flushing.isSrcDone = 1;
                }
            }
            //dst
            if(ctx->flushing.isSrcDone && !ctx->flushing.isCompleted){
                //only flush src
                ctx->flushing.isCompleted = 1;
                //K_LOG_INFO("StreamContext, flushing completed (%ums) (at '%s').\n", ctx->flushing.msAccum, srcLocation);
                /*
                //try to dequeue bufer
                while(ctx->dec.dst.enqueuedCount > 0){
                    STBuffer* buff = NULL;
                    if(0 != Buffers_dequeue(&ctx->dec.dst, ctx->dec.fd, &buff, NULL)){
                        buff = NULL;
                        break;
                    }
                }
                if(ctx->dec.dst.enqueuedCount == 0){
                    K_LOG_INFO("StreamContext, flushing src-dst-completed (%ums) (at '%s').\n", ctx->flushing.msAccum, srcLocation);
                    ctx->flushing.isCompleted = 1;
                }*/
            }
            //timeout
            if(ctx->flushing.msAccum > (250) && (!ctx->flushing.isSrcDone || !ctx->flushing.isCompleted)){
                K_LOG_WARN("StreamContext, flushing timeout(%ums), %d buffers still in src-queue and %d in dst-queue (at '%s').\n", ctx->flushing.msAccum, ctx->dec.src.enqueuedCount, ctx->dec.dst.enqueuedCount, srcLocation);
                ctx->flushing.isSrcDone = 1;
                ctx->flushing.isCompleted = 1;
            }
        }
    }
    return r;
}

int StreamContext_shutdownStart(STStreamContext* ctx, struct STPlayer_* plyr, const int isPermanent){
    int r = -1;
    if(!ctx->shuttingDown.isActive){
        r = 0;
        //activate
        memset(&ctx->shuttingDown, 0, sizeof(ctx->shuttingDown));
        ctx->shuttingDown.isActive = 1;
        ctx->shuttingDown.isPermanent = isPermanent;
        //activate flush
        if(!ctx->flushing.isActive){
            if(0 != StreamContext_flushStart(ctx)){
                K_LOG_ERROR("StreamContext_shutdownStart, StreamContext_flushStart failed.\n");
                r = -1;
            }
        }
        if(ctx->dec.fd < 0){
            //K_LOG_INFO("StreamContext, shutdown starting and completed (no decoder open).\n");
            ctx->shuttingDown.isCompleted = 1;
        } else {
            //K_LOG_INFO("StreamContext, shutdown starting.\n");
            //shutdown-tick once to porgress now if posible
            if(0 != StreamContext_shutdownTick(ctx, plyr, 0, "StreamContext_shutdownStart")){
                K_LOG_ERROR("StreamContext_shutdownStart, StreamContext_shutdownTick failed.\n");
                r = -1;
            }
        }
    }
    return r;
}

int StreamContext_shutdownTick(STStreamContext* ctx, struct STPlayer_* plyr, const int ms, const char* srcLocation){
    int r = -1;
    if(ctx->shuttingDown.isActive && !ctx->shuttingDown.isCompleted){
        r = 0;
        ctx->shuttingDown.msAccum += ms;
        //
        if(ctx->dec.fd < 0){
            if(!ctx->shuttingDown.isCompleted){
                K_LOG_WARN("StreamContext, device closed before shutting-down (%ums) (at '%s').\n", ctx->shuttingDown.msAccum, srcLocation);
                ctx->shuttingDown.isCompleted = 1;
            }
        } else {
            //
            if(ctx->flushing.isCompleted){
                //K_LOG_INFO("StreamContext, shutting-down-completed (%ums) (at '%s').\n", ctx->shuttingDown.msAccum, srcLocation);
                ctx->shuttingDown.isCompleted = 1;
                if(0 != StreamContext_close(ctx, plyr)){
                    K_LOG_WARN("StreamContext_close failed: '%s' (at shutdown completion) (at '%s').\n", ctx->cfg.path, srcLocation);
                } else {
                    K_LOG_VERBOSE("StreamContext(%lld), shutdown completed %ums: '%s' (at shutdown completion) (at '%s').\n", (long long)ctx, ctx->shuttingDown.msAccum, ctx->cfg.path, srcLocation);
                    K_ASSERT(ctx->dec.fd < 0)
                }
            }
            //timeout
            if(!ctx->shuttingDown.isCompleted && ctx->shuttingDown.msAccum > (500)){
                K_LOG_WARN("StreamContext, shutting-down timeout(%ums), %d buffers still in src-queue and %d in dst-queue (at '%s').\n", ctx->shuttingDown.msAccum, ctx->dec.src.enqueuedCount, ctx->dec.dst.enqueuedCount, srcLocation);
                ctx->shuttingDown.isCompleted = 1;
                if(0 != StreamContext_close(ctx, plyr)){
                    K_LOG_WARN("StreamContext_close failed: '%s' (at shutdown timeout) (at '%s').\n", ctx->cfg.path, srcLocation);
                }
            }
        }
    }
    return r;
}


void StreamContext_cnsmRespHttpHeader_(STStreamContext* ctx){
    while(!ctx->net.resp.headerEnded && ctx->buff.buffCsmd < ctx->buff.buffUse){
        //0 = [], 1 = ['\r'], 2 = ['\r', '\n'], 3 = ['\r', '\n', '\r']
        unsigned char c = ctx->buff.buff[ctx->buff.buffCsmd];
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
                    K_LOG_VERBOSE("StreamContext, net, response body started (after %d bytes header).\n", (ctx->net.resp.headerSz + 1));
                    //print
                    {
                        if((ctx->buff.buffCsmd + 1) < ctx->buff.buffSz){
                            unsigned char b = ctx->buff.buff[ctx->buff.buffCsmd + 1];
                            ctx->buff.buff[ctx->buff.buffCsmd + 1] = '\0';
                            K_LOG_VERBOSE("StreamContext, net, response header (last-read):\n-->%s<--.\n", &ctx->buff.buff[0]);
                            ctx->buff.buff[ctx->buff.buffCsmd + 1] = b;
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
        ctx->buff.buffCsmd++;
    }
}

int StreamContext_getPollEventsMask(STStreamContext* ctx){
    int events = POLLERR | POLLPRI;
    //src-events only if dst-is-runnning
    /*
    if(ctx->dec.src.isExplicitON && ctx->dec.src.isImplicitON && ctx->dec.dst.isExplicitON && ctx->dec.dst.isImplicitON){
        if(VideoFrames_getFramesForReadCount(&ctx->frames.filled) > 0){
            events |= POLLOUT | POLLWRNORM; //src
        }
        events |= POLLIN | POLLRDNORM; //dst
    }
    */
    //src-events and dst-events indepently
    if(ctx->dec.src.isExplicitON && ctx->dec.src.isImplicitON){
        if(!ctx->flushing.isActive && VideoFrames_getFramesForReadCount(&ctx->frames.filled) > 0){
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
        K_LOG_ERROR("poll-update-failed to '%s'.\n", ctx->cfg.path);
    } else {
        if(eventsBefore != events){
            //
            if((eventsBefore & (POLLERR)) != (events & (POLLERR))){
                K_LOG_VERBOSE("StreamContext, poll-mask: %cerr.\n", (eventsBefore & (POLLERR)) ? '-' : '+');
            }
            if((eventsBefore & (POLLPRI)) != (events & (POLLPRI))){
                K_LOG_VERBOSE("StreamContext, poll-mask: %cevents.\n", (eventsBefore & (POLLPRI)) ? '-' : '+');
            }
            if((eventsBefore & (POLLOUT | POLLWRNORM)) != (events & (POLLOUT | POLLWRNORM))){
                K_LOG_VERBOSE("StreamContext, poll-mask: %csrc.\n", (eventsBefore & (POLLOUT | POLLWRNORM)) ? '-' : '+');
            }
            if((eventsBefore & (POLLIN | POLLRDNORM)) != (events & (POLLIN | POLLRDNORM))){
                K_LOG_VERBOSE("StreamContext, poll-mask: %cdst.\n", (eventsBefore & (POLLIN | POLLRDNORM)) ? '-' : '+');
            }
            //
            K_LOG_VERBOSE("StreamContext, device-poll listening:%s%s%s%s%s.\n", (events & POLLERR) ? " errors" : "", (events & POLLPRI) ? " events": "", (events & (POLLOUT | POLLWRNORM)) ? " src": "", (events & (POLLIN | POLLRDNORM)) ? " dst": "", (events & (POLLERR | POLLPRI | POLLOUT | POLLWRNORM | POLLIN | POLLRDNORM)) == 0 ? "none" : "");
        }
    }
}

int StreamContext_getPollEventsMaskFile(STStreamContext* ctx, struct STPlayer_* plyr){
    int events = POLLERR;
    //src-events and dst-events indepently
    if(!ctx->flushing.isActive //not flushing src
       && (ctx->buff.screenRefreshSeqBlocking == 0 || ctx->buff.screenRefreshSeqBlocking != plyr->anim.tickSeq) //not waiting for screen change
       ){
        events |= POLLIN;
    }
    return events;
}

void StreamContext_updatePollMaskFile_(STStreamContext* ctx, struct STPlayer_* plyr){
    if(ctx->file.fd >= 0){
        const int mask = StreamContext_getPollEventsMaskFile(ctx, plyr);
        if(0 != Player_pollUpdate(plyr, ENPlayerPollFdType_SrcFile, ctx, ctx->file.fd, mask, NULL)){ //read
            const char* reason = "poll-update-failed";
            if(0 != StreamContext_shutdownStartByFileClosed_(ctx, plyr, reason)){
                K_LOG_ERROR("StreamContext, StreamContext_shutdownStart failed (at '%s').\n", reason);
            }
        }
    }
}

void StreamContext_cnsmFrameOportunity_(STStreamContext* ctx, struct STPlayer_* plyr){
    int tryAgain = 1;
    while(tryAgain && ctx->dec.fd >= 0 && !ctx->flushing.isActive && VideoFrames_getFramesForReadCount(&ctx->frames.filled) > 0){
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
        tryAgain = 0;
        if(buff == NULL){
            //
        } else {
            STVideoFrame* frame = NULL;
            if(0 != VideoFrames_pullFrameForRead(&ctx->frames.filled, &frame)){
                K_LOG_ERROR("StreamContext(%lld), no frame available (program logic error).\n", (long long)ctx);
            } else {
                //validate
                if(buff->planesSz < 0){
                    K_LOG_ERROR("StreamContext, at least one plane is required.\n");
                } else if(buff->planes[0].length < frame->buff.use){
                    K_LOG_ERROR("StreamContext, frame doesnt fit on plane's buffer.\n");
                } else if(ctx->dec.isWaitingForIDRFrame && !frame->state.isIndependent){
                    K_LOG_VERBOSE("StreamContext(%lld), frame(#%d) ignored, waiting-for-IDR, %d states-fed.\n", (long long)ctx, (frame->state.iSeq + 1), ctx->dec.frames.fed.use);
                    tryAgain = 1; //search next frame
                } else {
                    ctx->dec.frames.foundCount++;
                    //feed
                    if(ctx->dec.frames.foundCount <= ctx->cfg.framesSkip){
                        K_LOG_INFO("StreamContext, skipping-fed frame (#%d/%d) (user-param 'framesSkip').\n", ctx->dec.frames.foundCount, ctx->cfg.framesSkip);
                    } else if(ctx->flushing.isActive){
                        K_LOG_INFO("StreamContext, skipping-fed frame (#%d/%d) (flushing).\n", ctx->dec.frames.foundCount, ctx->cfg.framesSkip);
                    } else {
                        struct timeval vTimestamp; //virtual timestamp
                        memset(&vTimestamp, 0, sizeof(vTimestamp));
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
                            K_LOG_ERROR("StreamContext, frame could not be queued.\n");
                        } else {
                            ctx->dec.framesInSinceOpen++;
                            //first-frame-fed event
                            if(ctx->dec.framesInSinceOpen == 1){
                                ctx->dec.msFirstFrameFed = ctx->dec.msOpen;
                                if(ctx->dec.msFirstFrameFed >= 1000){
                                    K_LOG_WARN("StreamContext(%lld), %ums to fed first frame (with types:%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s).\n"
                                               , (long long)ctx, ctx->dec.msOpen
                                               , frame->accessUnit.nalsCountPerType[0] ? " 0" : ""
                                               , frame->accessUnit.nalsCountPerType[1] ? " 1" : ""
                                               , frame->accessUnit.nalsCountPerType[2] ? " 2" : ""
                                               , frame->accessUnit.nalsCountPerType[3] ? " 3" : ""
                                               , frame->accessUnit.nalsCountPerType[4] ? " 4" : ""
                                               , frame->accessUnit.nalsCountPerType[5] ? " 5" : ""
                                               , frame->accessUnit.nalsCountPerType[6] ? " 6" : ""
                                               , frame->accessUnit.nalsCountPerType[7] ? " 7" : ""
                                               , frame->accessUnit.nalsCountPerType[8] ? " 8" : ""
                                               , frame->accessUnit.nalsCountPerType[9] ? " 9" : ""
                                               , frame->accessUnit.nalsCountPerType[10] ? " 10" : ""
                                               , frame->accessUnit.nalsCountPerType[11] ? " 11" : ""
                                               , frame->accessUnit.nalsCountPerType[12] ? " 12" : ""
                                               , frame->accessUnit.nalsCountPerType[13] ? " 13" : ""
                                               , frame->accessUnit.nalsCountPerType[14] ? " 14" : ""
                                               , frame->accessUnit.nalsCountPerType[15] ? " 15" : ""
                                               , frame->accessUnit.nalsCountPerType[16] ? " 16" : ""
                                               , frame->accessUnit.nalsCountPerType[17] ? " 17" : ""
                                               , frame->accessUnit.nalsCountPerType[18] ? " 18" : ""
                                               , frame->accessUnit.nalsCountPerType[19] ? " 19" : ""
                                               , frame->accessUnit.nalsCountPerType[20] ? " 20" : ""
                                               , frame->accessUnit.nalsCountPerType[21] ? " 21" : ""
                                               , frame->accessUnit.nalsCountPerType[22] ? " 22" : ""
                                               , frame->accessUnit.nalsCountPerType[23] ? " 23" : ""
                                               , frame->accessUnit.nalsCountPerType[24] ? " 24" : ""
                                               , frame->accessUnit.nalsCountPerType[25] ? " 25" : ""
                                               , frame->accessUnit.nalsCountPerType[26] ? " 26" : ""
                                               , frame->accessUnit.nalsCountPerType[27] ? " 27" : ""
                                               , frame->accessUnit.nalsCountPerType[28] ? " 28" : ""
                                               , frame->accessUnit.nalsCountPerType[29] ? " 29" : ""
                                               , frame->accessUnit.nalsCountPerType[30] ? " 30" : ""
                                               , frame->accessUnit.nalsCountPerType[31] ? " 31" : ""
                                               );
                                } else {
                                    K_LOG_VERBOSE("StreamContext(%lld), %ums to fed first frame (with types:%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s).\n"
                                               , (long long)ctx, ctx->dec.msOpen
                                               , frame->accessUnit.nalsCountPerType[0] ? " 0" : ""
                                               , frame->accessUnit.nalsCountPerType[1] ? " 1" : ""
                                               , frame->accessUnit.nalsCountPerType[2] ? " 2" : ""
                                               , frame->accessUnit.nalsCountPerType[3] ? " 3" : ""
                                               , frame->accessUnit.nalsCountPerType[4] ? " 4" : ""
                                               , frame->accessUnit.nalsCountPerType[5] ? " 5" : ""
                                               , frame->accessUnit.nalsCountPerType[6] ? " 6" : ""
                                               , frame->accessUnit.nalsCountPerType[7] ? " 7" : ""
                                               , frame->accessUnit.nalsCountPerType[8] ? " 8" : ""
                                               , frame->accessUnit.nalsCountPerType[9] ? " 9" : ""
                                               , frame->accessUnit.nalsCountPerType[10] ? " 10" : ""
                                               , frame->accessUnit.nalsCountPerType[11] ? " 11" : ""
                                               , frame->accessUnit.nalsCountPerType[12] ? " 12" : ""
                                               , frame->accessUnit.nalsCountPerType[13] ? " 13" : ""
                                               , frame->accessUnit.nalsCountPerType[14] ? " 14" : ""
                                               , frame->accessUnit.nalsCountPerType[15] ? " 15" : ""
                                               , frame->accessUnit.nalsCountPerType[16] ? " 16" : ""
                                               , frame->accessUnit.nalsCountPerType[17] ? " 17" : ""
                                               , frame->accessUnit.nalsCountPerType[18] ? " 18" : ""
                                               , frame->accessUnit.nalsCountPerType[19] ? " 19" : ""
                                               , frame->accessUnit.nalsCountPerType[20] ? " 20" : ""
                                               , frame->accessUnit.nalsCountPerType[21] ? " 21" : ""
                                               , frame->accessUnit.nalsCountPerType[22] ? " 22" : ""
                                               , frame->accessUnit.nalsCountPerType[23] ? " 23" : ""
                                               , frame->accessUnit.nalsCountPerType[24] ? " 24" : ""
                                               , frame->accessUnit.nalsCountPerType[25] ? " 25" : ""
                                               , frame->accessUnit.nalsCountPerType[26] ? " 26" : ""
                                               , frame->accessUnit.nalsCountPerType[27] ? " 27" : ""
                                               , frame->accessUnit.nalsCountPerType[28] ? " 28" : ""
                                               , frame->accessUnit.nalsCountPerType[29] ? " 29" : ""
                                               , frame->accessUnit.nalsCountPerType[30] ? " 30" : ""
                                               , frame->accessUnit.nalsCountPerType[31] ? " 31" : ""
                                               );
                                }
                            }
                            //Add frame state to processing queue
                            VideoFrameStates_addNewestCloning(&ctx->dec.frames.fed, &frame->state);
                            ctx->dec.msWithoutFeedFrame = 0;
                            ctx->dec.isWaitingForIDRFrame = 0;
                            //stats
                            plyr->stats.curSec.dec.fed.count++;
                            //
                            tryAgain = 1; //search next frame
                            ctx->dec.frames.fedCount++;
                            K_LOG_VERBOSE("StreamContext, frame(#%d, with types:%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s) queued to src-buffs (%s), %d states-fed.\n"
                                          , (frame->state.iSeq + 1)
                                          , frame->accessUnit.nalsCountPerType[0] ? " 0" : ""
                                          , frame->accessUnit.nalsCountPerType[1] ? " 1" : ""
                                          , frame->accessUnit.nalsCountPerType[2] ? " 2" : ""
                                          , frame->accessUnit.nalsCountPerType[3] ? " 3" : ""
                                          , frame->accessUnit.nalsCountPerType[4] ? " 4" : ""
                                          , frame->accessUnit.nalsCountPerType[5] ? " 5" : ""
                                          , frame->accessUnit.nalsCountPerType[6] ? " 6" : ""
                                          , frame->accessUnit.nalsCountPerType[7] ? " 7" : ""
                                          , frame->accessUnit.nalsCountPerType[8] ? " 8" : ""
                                          , frame->accessUnit.nalsCountPerType[9] ? " 9" : ""
                                          , frame->accessUnit.nalsCountPerType[10] ? " 10" : ""
                                          , frame->accessUnit.nalsCountPerType[11] ? " 11" : ""
                                          , frame->accessUnit.nalsCountPerType[12] ? " 12" : ""
                                          , frame->accessUnit.nalsCountPerType[13] ? " 13" : ""
                                          , frame->accessUnit.nalsCountPerType[14] ? " 14" : ""
                                          , frame->accessUnit.nalsCountPerType[15] ? " 15" : ""
                                          , frame->accessUnit.nalsCountPerType[16] ? " 16" : ""
                                          , frame->accessUnit.nalsCountPerType[17] ? " 17" : ""
                                          , frame->accessUnit.nalsCountPerType[18] ? " 18" : ""
                                          , frame->accessUnit.nalsCountPerType[19] ? " 19" : ""
                                          , frame->accessUnit.nalsCountPerType[20] ? " 20" : ""
                                          , frame->accessUnit.nalsCountPerType[21] ? " 21" : ""
                                          , frame->accessUnit.nalsCountPerType[22] ? " 22" : ""
                                          , frame->accessUnit.nalsCountPerType[23] ? " 23" : ""
                                          , frame->accessUnit.nalsCountPerType[24] ? " 24" : ""
                                          , frame->accessUnit.nalsCountPerType[25] ? " 25" : ""
                                          , frame->accessUnit.nalsCountPerType[26] ? " 26" : ""
                                          , frame->accessUnit.nalsCountPerType[27] ? " 27" : ""
                                          , frame->accessUnit.nalsCountPerType[28] ? " 28" : ""
                                          , frame->accessUnit.nalsCountPerType[29] ? " 29" : ""
                                          , frame->accessUnit.nalsCountPerType[30] ? " 30" : ""
                                          , frame->accessUnit.nalsCountPerType[31] ? " 31" : ""
                                          , (buffIsDequeued ? "dequeued" : "unused"), ctx->dec.frames.fed.use);
                            //start shutdown after this frame
                            if(ctx->cfg.framesFeedMax > 0 && ctx->dec.frames.fedCount >= ctx->cfg.framesFeedMax && !ctx->shuttingDown.isActive){
                                const int isPermanent = 1;
                                if(0 != StreamContext_shutdownStart(ctx, plyr, isPermanent)){
                                    K_LOG_ERROR("StreamContext, StreamContext_shutdownStart failed.\n");
                                } else {
                                    K_LOG_INFO("StreamContext, StreamContext_shutdownStart after %d frames fed (user-param 'framesFeedMax').\n", ctx->dec.frames.fedCount);
                                }
                            }
                        }
                    }
                }
                //reuse
                if(0 != VideoFrames_pushFrameOwning(&ctx->frames.reusable, frame)){
                    K_LOG_ERROR("StreamContext, frame could not be returned to reusable.\n");
                } else {
                    frame = NULL; //consume
                }
                //release (if not consumed)
                if(frame != NULL){
                    free(frame);
                    frame = NULL;
                }
                //update poll (last filled-frame was consumed)
                if(VideoFrames_getFramesForReadCount(&ctx->frames.filled) <= 0){
                    StreamContext_updatePollMask_(ctx, plyr);
                }
            }
        }
    }
}

void StreamContext_cnsmBuffNALOpenNewFilling_(STStreamContext* ctx, struct STPlayer_* plyr, const int flushOldersIfIsIndependent, const int nalType, const int keepCurNalInCurFrame, int* dstFilledAdded){
    STVideoFrame* frame = NULL;
    int filledAdded = 0;
    if(0 != VideoFrames_pullFrameForFill(&ctx->frames.reusable, &frame)){
        K_LOG_INFO("StreamContext, VideoFrames_pullFrameForFill failed.\n");
        frame = NULL;
    } else {
        int fillingCarryAheadSz = (keepCurNalInCurFrame ? 0 : ctx->frames.fillingNalSz);
        //initial state
        gettimeofday(&frame->state.times.arrival.start, NULL);
        gettimeofday(&frame->state.times.arrival.end, NULL);
        gettimeofday(&frame->state.times.proc.start, NULL);
        gettimeofday(&frame->state.times.proc.end, NULL);
        //carry last partially-copied-nal to next frame
        if(fillingCarryAheadSz > 0){
            K_ASSERT(ctx->frames.filling->buff.use >= fillingCarryAheadSz)
            if(0 != VideoFrame_copy(frame, &ctx->frames.filling->buff.ptr[ctx->frames.filling->buff.use - fillingCarryAheadSz], fillingCarryAheadSz)){
                K_LOG_ERROR("VideoFrame_copy failed.\n");
                VideoFrame_release(ctx->frames.filling);
                free(ctx->frames.filling);
                ctx->frames.filling = NULL;
                ctx->frames.fillingNalSz = 0;
                //
                fillingCarryAheadSz = 0;
            } else {
                //consume
                K_ASSERT(frame->buff.use == fillingCarryAheadSz);
                ctx->frames.filling->buff.use -= fillingCarryAheadSz;
                //carry nalTypeCount from current frame (already counted) to next frame
                {
                    K_ASSERT(nalType >= 0 && nalType < 32)
                    if(nalType >= 0 && nalType < 32){
                        K_ASSERT(ctx->frames.filling->accessUnit.nalsCountPerType[nalType] > 0)
                        if(ctx->frames.filling->accessUnit.nalsCountPerType[nalType] > 0){
                            ctx->frames.filling->accessUnit.nalsCountPerType[nalType]--;
                        }
                        K_ASSERT(frame->accessUnit.nalsCountPerType[nalType] == 0)
                        frame->accessUnit.nalsCountPerType[nalType]++;
                    }
                }
            }
        }
        //add to 'filled' or 'reuse' queue
        if(ctx->frames.filling != NULL){
            int addedIsIDR = 0;
            if(ctx->frames.filling->buff.use <= 0){
                K_LOG_WARN("StreamContext, ignoring zero-size frame(#%d, %d bytes, types:%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s) completed (%d filled-frames in queue).\n", (ctx->frames.filling->state.iSeq + 1), ctx->frames.filling->buff.use, ctx->frames.filling->accessUnit.nalsCountPerType[0] ? " 0" : "", ctx->frames.filling->accessUnit.nalsCountPerType[1] ? " 1" : "", ctx->frames.filling->accessUnit.nalsCountPerType[2] ? " 2" : "", ctx->frames.filling->accessUnit.nalsCountPerType[3] ? " 3" : "", ctx->frames.filling->accessUnit.nalsCountPerType[4] ? " 4" : "", ctx->frames.filling->accessUnit.nalsCountPerType[5] ? " 5" : "", ctx->frames.filling->accessUnit.nalsCountPerType[6] ? " 6" : "", ctx->frames.filling->accessUnit.nalsCountPerType[7] ? " 7" : "", ctx->frames.filling->accessUnit.nalsCountPerType[8] ? " 8" : "", ctx->frames.filling->accessUnit.nalsCountPerType[9] ? " 9" : "", ctx->frames.filling->accessUnit.nalsCountPerType[10] ? " 10" : "", ctx->frames.filling->accessUnit.nalsCountPerType[11] ? " 11" : "", ctx->frames.filling->accessUnit.nalsCountPerType[12] ? " 12" : "", ctx->frames.filling->accessUnit.nalsCountPerType[13] ? " 13" : "", ctx->frames.filling->accessUnit.nalsCountPerType[14] ? " 14" : "", ctx->frames.filling->accessUnit.nalsCountPerType[15] ? " 15" : "", ctx->frames.filling->accessUnit.nalsCountPerType[16] ? " 16" : "", ctx->frames.filling->accessUnit.nalsCountPerType[17] ? " 17" : "", ctx->frames.filling->accessUnit.nalsCountPerType[18] ? " 18" : "", ctx->frames.filling->accessUnit.nalsCountPerType[19] ? " 19" : "", ctx->frames.filling->accessUnit.nalsCountPerType[20] ? " 20" : "", ctx->frames.filling->accessUnit.nalsCountPerType[21] ? " 21" : "", ctx->frames.filling->accessUnit.nalsCountPerType[22] ? " 22" : "", ctx->frames.filling->accessUnit.nalsCountPerType[23] ? " 23" : "", ctx->frames.filling->accessUnit.nalsCountPerType[24] ? " 24" : "", ctx->frames.filling->accessUnit.nalsCountPerType[25] ? " 25" : "", ctx->frames.filling->accessUnit.nalsCountPerType[26] ? " 26" : "", ctx->frames.filling->accessUnit.nalsCountPerType[27] ? " 27" : "", ctx->frames.filling->accessUnit.nalsCountPerType[28] ? " 28" : "", ctx->frames.filling->accessUnit.nalsCountPerType[29] ? " 29" : "", ctx->frames.filling->accessUnit.nalsCountPerType[30] ? " 30" : "", ctx->frames.filling->accessUnit.nalsCountPerType[31] ? " 31" : "", ctx->frames.filled.use);
            } else if(ctx->frames.filling->accessUnit.isInvalid){
                K_LOG_WARN("StreamContext, ignoring explicit-invalidated frame(#%d, %d bytes, types:%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s) completed (%d filled-frames in queue).\n", (ctx->frames.filling->state.iSeq + 1), ctx->frames.filling->buff.use, ctx->frames.filling->accessUnit.nalsCountPerType[0] ? " 0" : "", ctx->frames.filling->accessUnit.nalsCountPerType[1] ? " 1" : "", ctx->frames.filling->accessUnit.nalsCountPerType[2] ? " 2" : "", ctx->frames.filling->accessUnit.nalsCountPerType[3] ? " 3" : "", ctx->frames.filling->accessUnit.nalsCountPerType[4] ? " 4" : "", ctx->frames.filling->accessUnit.nalsCountPerType[5] ? " 5" : "", ctx->frames.filling->accessUnit.nalsCountPerType[6] ? " 6" : "", ctx->frames.filling->accessUnit.nalsCountPerType[7] ? " 7" : "", ctx->frames.filling->accessUnit.nalsCountPerType[8] ? " 8" : "", ctx->frames.filling->accessUnit.nalsCountPerType[9] ? " 9" : "", ctx->frames.filling->accessUnit.nalsCountPerType[10] ? " 10" : "", ctx->frames.filling->accessUnit.nalsCountPerType[11] ? " 11" : "", ctx->frames.filling->accessUnit.nalsCountPerType[12] ? " 12" : "", ctx->frames.filling->accessUnit.nalsCountPerType[13] ? " 13" : "", ctx->frames.filling->accessUnit.nalsCountPerType[14] ? " 14" : "", ctx->frames.filling->accessUnit.nalsCountPerType[15] ? " 15" : "", ctx->frames.filling->accessUnit.nalsCountPerType[16] ? " 16" : "", ctx->frames.filling->accessUnit.nalsCountPerType[17] ? " 17" : "", ctx->frames.filling->accessUnit.nalsCountPerType[18] ? " 18" : "", ctx->frames.filling->accessUnit.nalsCountPerType[19] ? " 19" : "", ctx->frames.filling->accessUnit.nalsCountPerType[20] ? " 20" : "", ctx->frames.filling->accessUnit.nalsCountPerType[21] ? " 21" : "", ctx->frames.filling->accessUnit.nalsCountPerType[22] ? " 22" : "", ctx->frames.filling->accessUnit.nalsCountPerType[23] ? " 23" : "", ctx->frames.filling->accessUnit.nalsCountPerType[24] ? " 24" : "", ctx->frames.filling->accessUnit.nalsCountPerType[25] ? " 25" : "", ctx->frames.filling->accessUnit.nalsCountPerType[26] ? " 26" : "", ctx->frames.filling->accessUnit.nalsCountPerType[27] ? " 27" : "", ctx->frames.filling->accessUnit.nalsCountPerType[28] ? " 28" : "", ctx->frames.filling->accessUnit.nalsCountPerType[29] ? " 29" : "", ctx->frames.filling->accessUnit.nalsCountPerType[30] ? " 30" : "", ctx->frames.filling->accessUnit.nalsCountPerType[31] ? " 31" : "", ctx->frames.filled.use);
            } else if(VideoFrame_getNalsCountOfGrp(ctx->frames.filling, ENNalTypeGrp_VCL) <= 0){
                K_LOG_WARN("StreamContext, ignoring zero-VCL frame(#%d, %d bytes, types:%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s) completed (%d filled-frames in queue).\n", (ctx->frames.filling->state.iSeq + 1), ctx->frames.filling->buff.use, ctx->frames.filling->accessUnit.nalsCountPerType[0] ? " 0" : "", ctx->frames.filling->accessUnit.nalsCountPerType[1] ? " 1" : "", ctx->frames.filling->accessUnit.nalsCountPerType[2] ? " 2" : "", ctx->frames.filling->accessUnit.nalsCountPerType[3] ? " 3" : "", ctx->frames.filling->accessUnit.nalsCountPerType[4] ? " 4" : "", ctx->frames.filling->accessUnit.nalsCountPerType[5] ? " 5" : "", ctx->frames.filling->accessUnit.nalsCountPerType[6] ? " 6" : "", ctx->frames.filling->accessUnit.nalsCountPerType[7] ? " 7" : "", ctx->frames.filling->accessUnit.nalsCountPerType[8] ? " 8" : "", ctx->frames.filling->accessUnit.nalsCountPerType[9] ? " 9" : "", ctx->frames.filling->accessUnit.nalsCountPerType[10] ? " 10" : "", ctx->frames.filling->accessUnit.nalsCountPerType[11] ? " 11" : "", ctx->frames.filling->accessUnit.nalsCountPerType[12] ? " 12" : "", ctx->frames.filling->accessUnit.nalsCountPerType[13] ? " 13" : "", ctx->frames.filling->accessUnit.nalsCountPerType[14] ? " 14" : "", ctx->frames.filling->accessUnit.nalsCountPerType[15] ? " 15" : "", ctx->frames.filling->accessUnit.nalsCountPerType[16] ? " 16" : "", ctx->frames.filling->accessUnit.nalsCountPerType[17] ? " 17" : "", ctx->frames.filling->accessUnit.nalsCountPerType[18] ? " 18" : "", ctx->frames.filling->accessUnit.nalsCountPerType[19] ? " 19" : "", ctx->frames.filling->accessUnit.nalsCountPerType[20] ? " 20" : "", ctx->frames.filling->accessUnit.nalsCountPerType[21] ? " 21" : "", ctx->frames.filling->accessUnit.nalsCountPerType[22] ? " 22" : "", ctx->frames.filling->accessUnit.nalsCountPerType[23] ? " 23" : "", ctx->frames.filling->accessUnit.nalsCountPerType[24] ? " 24" : "", ctx->frames.filling->accessUnit.nalsCountPerType[25] ? " 25" : "", ctx->frames.filling->accessUnit.nalsCountPerType[26] ? " 26" : "", ctx->frames.filling->accessUnit.nalsCountPerType[27] ? " 27" : "", ctx->frames.filling->accessUnit.nalsCountPerType[28] ? " 28" : "", ctx->frames.filling->accessUnit.nalsCountPerType[29] ? " 29" : "", ctx->frames.filling->accessUnit.nalsCountPerType[30] ? " 30" : "", ctx->frames.filling->accessUnit.nalsCountPerType[31] ? " 31" : "", ctx->frames.filled.use);
            } else if(
                      !(ctx->frames.filling->accessUnit.nalsCountPerType[0] == 0
                        && ctx->frames.filling->accessUnit.nalsCountPerType[1] == 0
                        && ctx->frames.filling->accessUnit.nalsCountPerType[2] == 0
                        && ctx->frames.filling->accessUnit.nalsCountPerType[3] == 0
                        && ctx->frames.filling->accessUnit.nalsCountPerType[4] == 0
                        && ctx->frames.filling->accessUnit.nalsCountPerType[5] == 1
                        && ctx->frames.filling->accessUnit.nalsCountPerType[6] <= 1 //allow SEI (some old cameras includes it)
                        && ctx->frames.filling->accessUnit.nalsCountPerType[7] == 1
                        && ctx->frames.filling->accessUnit.nalsCountPerType[8] == 1
                        && ctx->frames.filling->accessUnit.nalsCountPerType[9] == 0
                        && ctx->frames.filling->accessUnit.nalsCountPerType[10] == 0
                        && ctx->frames.filling->accessUnit.nalsCountPerType[11] == 0
                        && ctx->frames.filling->accessUnit.nalsCountPerType[12] == 0
                        && ctx->frames.filling->accessUnit.nalsCountPerType[13] == 0
                        && ctx->frames.filling->accessUnit.nalsCountPerType[14] == 0
                        && ctx->frames.filling->accessUnit.nalsCountPerType[15] == 0
                        && ctx->frames.filling->accessUnit.nalsCountPerType[16] == 0
                        && ctx->frames.filling->accessUnit.nalsCountPerType[17] == 0
                        && ctx->frames.filling->accessUnit.nalsCountPerType[18] == 0
                        && ctx->frames.filling->accessUnit.nalsCountPerType[19] == 0
                        && ctx->frames.filling->accessUnit.nalsCountPerType[20] == 0
                        && ctx->frames.filling->accessUnit.nalsCountPerType[21] == 0
                        && ctx->frames.filling->accessUnit.nalsCountPerType[22] == 0
                        && ctx->frames.filling->accessUnit.nalsCountPerType[23] == 0
                        && ctx->frames.filling->accessUnit.nalsCountPerType[24] == 0
                        && ctx->frames.filling->accessUnit.nalsCountPerType[25] == 0
                        && ctx->frames.filling->accessUnit.nalsCountPerType[26] == 0
                        && ctx->frames.filling->accessUnit.nalsCountPerType[27] == 0
                        && ctx->frames.filling->accessUnit.nalsCountPerType[28] == 0
                        && ctx->frames.filling->accessUnit.nalsCountPerType[29] == 0
                        && ctx->frames.filling->accessUnit.nalsCountPerType[30] == 0
                        && ctx->frames.filling->accessUnit.nalsCountPerType[31] == 0
                      )
                && !(ctx->frames.filling->accessUnit.nalsCountPerType[0] == 0
                     && ctx->frames.filling->accessUnit.nalsCountPerType[1] == 1
                     && ctx->frames.filling->accessUnit.nalsCountPerType[2] == 0
                     && ctx->frames.filling->accessUnit.nalsCountPerType[3] == 0
                     && ctx->frames.filling->accessUnit.nalsCountPerType[4] == 0
                     && ctx->frames.filling->accessUnit.nalsCountPerType[5] == 0
                     && ctx->frames.filling->accessUnit.nalsCountPerType[6] == 0
                     && ctx->frames.filling->accessUnit.nalsCountPerType[7] == 0
                     && ctx->frames.filling->accessUnit.nalsCountPerType[8] == 0
                     && ctx->frames.filling->accessUnit.nalsCountPerType[9] == 0
                     && ctx->frames.filling->accessUnit.nalsCountPerType[10] == 0
                     && ctx->frames.filling->accessUnit.nalsCountPerType[11] == 0
                     && ctx->frames.filling->accessUnit.nalsCountPerType[12] == 0
                     && ctx->frames.filling->accessUnit.nalsCountPerType[13] == 0
                     && ctx->frames.filling->accessUnit.nalsCountPerType[14] == 0
                     && ctx->frames.filling->accessUnit.nalsCountPerType[15] == 0
                     && ctx->frames.filling->accessUnit.nalsCountPerType[16] == 0
                     && ctx->frames.filling->accessUnit.nalsCountPerType[17] == 0
                     && ctx->frames.filling->accessUnit.nalsCountPerType[18] == 0
                     && ctx->frames.filling->accessUnit.nalsCountPerType[19] == 0
                     && ctx->frames.filling->accessUnit.nalsCountPerType[20] == 0
                     && ctx->frames.filling->accessUnit.nalsCountPerType[21] == 0
                     && ctx->frames.filling->accessUnit.nalsCountPerType[22] == 0
                     && ctx->frames.filling->accessUnit.nalsCountPerType[23] == 0
                     && ctx->frames.filling->accessUnit.nalsCountPerType[24] == 0
                     && ctx->frames.filling->accessUnit.nalsCountPerType[25] == 0
                     && ctx->frames.filling->accessUnit.nalsCountPerType[26] == 0
                     && ctx->frames.filling->accessUnit.nalsCountPerType[27] == 0
                     && ctx->frames.filling->accessUnit.nalsCountPerType[28] == 0
                     && ctx->frames.filling->accessUnit.nalsCountPerType[29] == 0
                     && ctx->frames.filling->accessUnit.nalsCountPerType[30] == 0
                     && ctx->frames.filling->accessUnit.nalsCountPerType[31] == 0
                     )
            )
            {
                //Tmp-quick-fix: allow only frames with [8, 7, 5] or [1] NALs.
                K_LOG_WARN("StreamContext, (tmp-quick-fix) ignoring frame not [8, 7, 5] or [1] (#%d, %d bytes, types:%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s) completed (%d filled-frames in queue).\n", (ctx->frames.filling->state.iSeq + 1), ctx->frames.filling->buff.use, ctx->frames.filling->accessUnit.nalsCountPerType[0] ? " 0" : "", ctx->frames.filling->accessUnit.nalsCountPerType[1] ? " 1" : "", ctx->frames.filling->accessUnit.nalsCountPerType[2] ? " 2" : "", ctx->frames.filling->accessUnit.nalsCountPerType[3] ? " 3" : "", ctx->frames.filling->accessUnit.nalsCountPerType[4] ? " 4" : "", ctx->frames.filling->accessUnit.nalsCountPerType[5] ? " 5" : "", ctx->frames.filling->accessUnit.nalsCountPerType[6] ? " 6" : "", ctx->frames.filling->accessUnit.nalsCountPerType[7] ? " 7" : "", ctx->frames.filling->accessUnit.nalsCountPerType[8] ? " 8" : "", ctx->frames.filling->accessUnit.nalsCountPerType[9] ? " 9" : "", ctx->frames.filling->accessUnit.nalsCountPerType[10] ? " 10" : "", ctx->frames.filling->accessUnit.nalsCountPerType[11] ? " 11" : "", ctx->frames.filling->accessUnit.nalsCountPerType[12] ? " 12" : "", ctx->frames.filling->accessUnit.nalsCountPerType[13] ? " 13" : "", ctx->frames.filling->accessUnit.nalsCountPerType[14] ? " 14" : "", ctx->frames.filling->accessUnit.nalsCountPerType[15] ? " 15" : "", ctx->frames.filling->accessUnit.nalsCountPerType[16] ? " 16" : "", ctx->frames.filling->accessUnit.nalsCountPerType[17] ? " 17" : "", ctx->frames.filling->accessUnit.nalsCountPerType[18] ? " 18" : "", ctx->frames.filling->accessUnit.nalsCountPerType[19] ? " 19" : "", ctx->frames.filling->accessUnit.nalsCountPerType[20] ? " 20" : "", ctx->frames.filling->accessUnit.nalsCountPerType[21] ? " 21" : "", ctx->frames.filling->accessUnit.nalsCountPerType[22] ? " 22" : "", ctx->frames.filling->accessUnit.nalsCountPerType[23] ? " 23" : "", ctx->frames.filling->accessUnit.nalsCountPerType[24] ? " 24" : "", ctx->frames.filling->accessUnit.nalsCountPerType[25] ? " 25" : "", ctx->frames.filling->accessUnit.nalsCountPerType[26] ? " 26" : "", ctx->frames.filling->accessUnit.nalsCountPerType[27] ? " 27" : "", ctx->frames.filling->accessUnit.nalsCountPerType[28] ? " 28" : "", ctx->frames.filling->accessUnit.nalsCountPerType[29] ? " 29" : "", ctx->frames.filling->accessUnit.nalsCountPerType[30] ? " 30" : "", ctx->frames.filling->accessUnit.nalsCountPerType[31] ? " 31" : "", ctx->frames.filled.use);
            } else {
                long msToArrive = 0;
                //final state
                {
                    gettimeofday(&ctx->frames.filling->state.times.arrival.end, NULL);
                    msToArrive = msBetweenTimevals(&ctx->frames.filling->state.times.arrival.start, &ctx->frames.filling->state.times.arrival.end);
                    addedIsIDR = ctx->frames.filling->state.isIndependent = (ctx->frames.filling->accessUnit.nalsCountPerType[5] > 0 ? 1 : 0); //IDR-Picture
                    //flush queue (if independent frame arrived)
                    if(flushOldersIfIsIndependent && ctx->frames.filling->state.isIndependent){
                        int skippedCount = 0;
                        STVideoFrame* frame = NULL;
                        while(0 == VideoFrames_pullFrameForRead(&ctx->frames.filled, &frame)){
                            skippedCount++;
                            //reuse
                            if(0 != VideoFrames_pushFrameOwning(&ctx->frames.reusable, frame)){
                                K_LOG_ERROR("StreamContext, frame could not be returned to reusable.\n");
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
                            K_LOG_VERBOSE("StreamContext(%lld), %d frames skipped (independent frame arrived).\n", ctx, skippedCount);
                        }
                    }
                }
                //add
                if(0 != VideoFrames_pushFrameOwning(&ctx->frames.filled, ctx->frames.filling)){
                    K_LOG_ERROR("VideoFrames_pushFrameOwning failed.\n");
                } else {
                    //K_LOG_INFO("StreamContext, Frame(#%d, %d bytes, %dms, types:%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s) completed (%d filled-frames in queue).\n", (ctx->frames.filling->state.iSeq + 1), ctx->frames.filling->buff.use, msToArrive, ctx->frames.filling->accessUnit.nalsCountPerType[0] ? " 0" : "", ctx->frames.filling->accessUnit.nalsCountPerType[1] ? " 1" : "", ctx->frames.filling->accessUnit.nalsCountPerType[2] ? " 2" : "", ctx->frames.filling->accessUnit.nalsCountPerType[3] ? " 3" : "", ctx->frames.filling->accessUnit.nalsCountPerType[4] ? " 4" : "", ctx->frames.filling->accessUnit.nalsCountPerType[5] ? " 5" : "", ctx->frames.filling->accessUnit.nalsCountPerType[6] ? " 6" : "", ctx->frames.filling->accessUnit.nalsCountPerType[7] ? " 7" : "", ctx->frames.filling->accessUnit.nalsCountPerType[8] ? " 8" : "", ctx->frames.filling->accessUnit.nalsCountPerType[9] ? " 9" : "", ctx->frames.filling->accessUnit.nalsCountPerType[10] ? " 10" : "", ctx->frames.filling->accessUnit.nalsCountPerType[11] ? " 11" : "", ctx->frames.filling->accessUnit.nalsCountPerType[12] ? " 12" : "", ctx->frames.filling->accessUnit.nalsCountPerType[13] ? " 13" : "", ctx->frames.filling->accessUnit.nalsCountPerType[14] ? " 14" : "", ctx->frames.filling->accessUnit.nalsCountPerType[15] ? " 15" : "", ctx->frames.filling->accessUnit.nalsCountPerType[16] ? " 16" : "", ctx->frames.filling->accessUnit.nalsCountPerType[17] ? " 17" : "", ctx->frames.filling->accessUnit.nalsCountPerType[18] ? " 18" : "", ctx->frames.filling->accessUnit.nalsCountPerType[19] ? " 19" : "", ctx->frames.filling->accessUnit.nalsCountPerType[20] ? " 20" : "", ctx->frames.filling->accessUnit.nalsCountPerType[21] ? " 21" : "", ctx->frames.filling->accessUnit.nalsCountPerType[22] ? " 22" : "", ctx->frames.filling->accessUnit.nalsCountPerType[23] ? " 23" : "", ctx->frames.filling->accessUnit.nalsCountPerType[24] ? " 24" : "", ctx->frames.filling->accessUnit.nalsCountPerType[25] ? " 25" : "", ctx->frames.filling->accessUnit.nalsCountPerType[26] ? " 26" : "", ctx->frames.filling->accessUnit.nalsCountPerType[27] ? " 27" : "", ctx->frames.filling->accessUnit.nalsCountPerType[28] ? " 28" : "", ctx->frames.filling->accessUnit.nalsCountPerType[29] ? " 29" : "", ctx->frames.filling->accessUnit.nalsCountPerType[30] ? " 30" : "", ctx->frames.filling->accessUnit.nalsCountPerType[31] ? " 31" : "", ctx->frames.filled.use);
                    ctx->frames.filling = NULL; //consume
                    ctx->frames.fillingNalSz = 0;
                    //
                    filledAdded = 1;
                }
            }
            //stats
            {
                if(filledAdded){
                    plyr->stats.curSec.src.frames.queued++;
                    if(addedIsIDR){
                        plyr->stats.curSec.src.frames.queuedIDR++;
                    }
                } else {
                    plyr->stats.curSec.src.frames.ignored++;
                }
            }
            //release (if not consumed)
            if(ctx->frames.filling != NULL){
                if(0 != VideoFrames_pushFrameOwning(&ctx->frames.reusable, ctx->frames.filling)){
                    K_LOG_ERROR("VideoFrames_pushFrameOwning failed.\n");
                } else {
                    ctx->frames.filling = NULL; //consume
                    ctx->frames.fillingNalSz = 0;
                }
            }
        }
        //release (if not consumed)
        if(ctx->frames.filling != NULL){
            VideoFrame_release(ctx->frames.filling);
            free(ctx->frames.filling);
            ctx->frames.filling = NULL;
            ctx->frames.fillingNalSz = 0;
        }
        //set
        ctx->frames.filling = frame; frame = NULL; //consume
        ctx->frames.fillingNalSz = fillingCarryAheadSz;
    }
    //release (if not consumed)
    if(frame != NULL){
        if(0 != VideoFrames_pushFrameOwning(&ctx->frames.reusable, frame)){
            K_LOG_ERROR("StreamContext, frame could not be returned to reusable.\n");
        } else {
            frame = NULL; //consume
        }
        //release (if not consumed)
        if(frame != NULL){
            VideoFrame_release(frame);
            free(frame);
            frame = NULL;
        }
    }
    //
    if(dstFilledAdded != NULL){
        *dstFilledAdded = filledAdded;
    }
}
    
void StreamContext_cnsmBuffNALChunk_(STStreamContext* ctx, struct STPlayer_* plyr, const int flushOldersIfIsIndependent, const unsigned char* data, const unsigned int dataSz, const int isEndOfNAL){
    const int filledWasEmpty = (VideoFrames_getFramesForReadCount(&ctx->frames.filled) <= 0 ? 1 : 0);
    int startNewFrame = 0, keepCurNalInCurFrame = 0; //default is to carry the NAL cahead
    int filledAddedBefore = 0, filledAddedAfter = 0;
    int nalType = 0, isNalTypeSet = 0;
    //
    if(ctx->frames.filling != NULL){
        K_ASSERT(ctx->frames.filling->buff.use >= ctx->frames.fillingNalSz)
        //analyze nalType
        if(ctx->frames.fillingNalSz >= 5){
            //obtain nalType from current buffer
            K_ASSERT(ctx->frames.filling->buff.use >= ctx->frames.fillingNalSz);
            K_ASSERT(ctx->frames.filling->buff.ptr[ctx->frames.filling->buff.use - ctx->frames.fillingNalSz + 0] == 0x00);
            K_ASSERT(ctx->frames.filling->buff.ptr[ctx->frames.filling->buff.use - ctx->frames.fillingNalSz + 1] == 0x00);
            K_ASSERT(ctx->frames.filling->buff.ptr[ctx->frames.filling->buff.use - ctx->frames.fillingNalSz + 2] == 0x00);
            K_ASSERT(ctx->frames.filling->buff.ptr[ctx->frames.filling->buff.use - ctx->frames.fillingNalSz + 3] == 0x01);
            //
            nalType = (ctx->frames.filling->buff.ptr[ctx->frames.filling->buff.use - ctx->frames.fillingNalSz + 4] & 0x1F);
            isNalTypeSet = 1;
            K_ASSERT(nalType >= 0 && nalType < 32)
            //K_LOG_INFO("StreamContext, nal-type(%d) continuing, frame(%d bytes, %u curNal).\n", nalType, ctx->frames.filling->buff.use, ctx->frames.fillingNalSz);
        } else if((ctx->frames.fillingNalSz + dataSz) >= 5){
            K_ASSERT(ctx->frames.filling->buff.use >= ctx->frames.fillingNalSz);
            K_ASSERT((ctx->frames.fillingNalSz > 0 ? ctx->frames.filling->buff.ptr[ctx->frames.filling->buff.use - ctx->frames.fillingNalSz + 0] : data[0 - ctx->frames.fillingNalSz]) == 0x00);
            K_ASSERT((ctx->frames.fillingNalSz > 1 ? ctx->frames.filling->buff.ptr[ctx->frames.filling->buff.use - ctx->frames.fillingNalSz + 1] : data[1 - ctx->frames.fillingNalSz]) == 0x00);
            K_ASSERT((ctx->frames.fillingNalSz > 2 ? ctx->frames.filling->buff.ptr[ctx->frames.filling->buff.use - ctx->frames.fillingNalSz + 2] : data[2 - ctx->frames.fillingNalSz]) == 0x00);
            K_ASSERT((ctx->frames.fillingNalSz > 3 ? ctx->frames.filling->buff.ptr[ctx->frames.filling->buff.use - ctx->frames.fillingNalSz + 3] : data[3 - ctx->frames.fillingNalSz]) == 0x01);
            //obtain nalType from input buffer (first time seen)
            nalType = (data[4 - ctx->frames.fillingNalSz] & 0x1F);
            isNalTypeSet = 1;
            //K_LOG_INFO("StreamContext, nal-type(%d) started, frame(%d bytes, %u curNal).\n", nalType, ctx->frames.filling->buff.use, ctx->frames.fillingNalSz);
            //analyze
            K_ASSERT(nalType >= 0 && nalType < 32)
            if(nalType >= 0 && nalType < 32){
                //Validate start of new access unit
                if(
                   nalType == 9 /*Access unit delimiter*/
                   ){
                       K_ASSERT((ctx->frames.filling->accessUnit.nalsCountPerType[9] && ctx->frames.filling->accessUnit.delimeter.isPresent) || (!ctx->frames.filling->accessUnit.nalsCountPerType[9] && !ctx->frames.filling->accessUnit.delimeter.isPresent)) //must match
                       if(!startNewFrame && VideoFrame_getNalsCount(ctx->frames.filling) > 0){
                           K_LOG_VERBOSE("StreamContext, nal-type(%d) opening new frame ('Access unit delimiter' at non-empty frame).\n", nalType);
                           startNewFrame = 1;
                           keepCurNalInCurFrame = 0;
                       }
                } else if(
                   nalType == 7 /*Sequence parameter set*/
                   || nalType == 8 /*Sequence parameter set*/
                   || nalType == 6 /*SEI*/
                   || (nalType >= 14 && nalType <= 18) /*...*/
                   //ToDo: implement '7.4.1.2.4 Detection of the first VCL NAL unit of a primary coded picture'.
                   //Asuming all VCL NALs are the first NAL of a primary coded.
                   || _naluDefs[nalType].grp == ENNalTypeGrp_VCL /*first VCL NAL unit of a primary coded picture*/
                   )
                {
                    if(!startNewFrame && VideoFrame_getNalsCountOfGrp(ctx->frames.filling, ENNalTypeGrp_VCL) > 0){
                        K_LOG_VERBOSE("StreamContext, opening new frame (nalType %d after the last VCL NAL).\n", nalType);
                        startNewFrame = 1;
                        keepCurNalInCurFrame = 0;
                    }
                }
                //Safety, this should be previouly validated at 'isEndOfNAL'
                K_ASSERT(ctx->frames.filling->accessUnit.nalsCountPerType[10] == 0)
                if(!startNewFrame && ctx->frames.filling->accessUnit.nalsCountPerType[10]){
                    K_LOG_VERBOSE("StreamContext, opening new frame ('End of sequence' already added).\n");
                    startNewFrame = 1;
                    keepCurNalInCurFrame = 0;
                }
                //constrains
                if(nalType == 13 /*Sequence parameter set extension*/){
                    if(ctx->frames.filling->accessUnit.lastCompletedNalType != 7 /*Sequence parameter set*/){
                        if(!ctx->frames.filling->accessUnit.isInvalid){
                            K_LOG_VERBOSE("StreamContext, invalidating frame ('Sequence parameter set extension' without inmediate-previous 'Sequence parameter set').\n");
                            ctx->frames.filling->accessUnit.isInvalid = 1;
                        }
                    }
                } else if(nalType == 19 /*coded slice of an auxiliary coded picture without partitioning*/){
                    const int nalVCLCount = VideoFrame_getNalsCountOfGrp(ctx->frames.filling, ENNalTypeGrp_VCL);
                    if(nalVCLCount == 0){
                        if(!ctx->frames.filling->accessUnit.isInvalid){
                            K_LOG_VERBOSE("StreamContext, invalidating frame ('auxiliary coded picture' without previous 'primary or redundant coded pictures').\n");
                            ctx->frames.filling->accessUnit.isInvalid = 1;
                        }
                    }
                } else if(nalType == 0 || nalType == 12 || (nalType >= 20 && nalType <= 31)){
                    if(!ctx->frames.filling->accessUnit.isInvalid && VideoFrame_getNalsCountOfGrp(ctx->frames.filling, ENNalTypeGrp_VCL) == 0){
                        K_LOG_VERBOSE("StreamContext, invalidating frame (nalType %d shall not precede the first VCL of the primary coded picture).\n", nalType);
                        ctx->frames.filling->accessUnit.isInvalid = 1;
                    }
                }
                //add
                ctx->frames.filling->accessUnit.nalsCountPerType[nalType]++;
            }
        }
    }
    //start new frame before copying data (carry cur-nal ahead)
    if(startNewFrame && !keepCurNalInCurFrame){
        K_ASSERT(isNalTypeSet)
        StreamContext_cnsmBuffNALOpenNewFilling_(ctx, plyr, flushOldersIfIsIndependent, nalType, keepCurNalInCurFrame, &filledAddedBefore);
    }
    //feed payload to frame
    if(ctx->frames.filling != NULL && dataSz > 0){
        if(0 != VideoFrame_copy(ctx->frames.filling, data, dataSz)){
            K_LOG_ERROR("VideoFrame_copy failed.\n");
            VideoFrame_release(ctx->frames.filling);
            free(ctx->frames.filling);
            ctx->frames.filling = NULL;
            ctx->frames.fillingNalSz = 0;
        } else {
            ctx->frames.fillingNalSz += dataSz;
        }
    }
    //analyze current frame after current NAL-end is confirmed
    if(isEndOfNAL){
        K_ASSERT(ctx->frames.fillingNalSz <= 4 || isNalTypeSet) //should be empty-nal or type-set
        if(ctx->frames.fillingNalSz <= 4){
            K_LOG_WARN("StreamContext, empty-nal found (%d bytes): '%s'.\n", ctx->frames.fillingNalSz, ctx->cfg.path);
            ctx->frames.filling->buff.use -= ctx->frames.fillingNalSz;
            ctx->frames.fillingNalSz -= ctx->frames.fillingNalSz;
        } else if(isNalTypeSet){
            if(ctx->frames.filling != NULL){
                ctx->frames.filling->accessUnit.lastCompletedNalType = nalType;
            }
            //
            if(isNalTypeSet && nalType == 10 /*End of sequence*/){
                K_ASSERT(ctx->frames.filling->accessUnit.nalsCountPerType[10]); //should be counted
                if(!startNewFrame){
                    //K_LOG_INFO("StreamContext, opening new frame ('End of sequence' completed).\n");
                    startNewFrame = 1;
                    keepCurNalInCurFrame = 1;
                }
            }
            //
            if(isNalTypeSet && nalType == 9 /*Access unit delimiter*/){
                K_ASSERT(ctx->frames.filling->accessUnit.nalsCountPerType[9]); //should be counted
                if(ctx->frames.fillingNalSz < 6){
                    K_LOG_ERROR("StreamContext, 'Access unit delimiter' should be 6 bytes or more (including header).\n");
                    if(!ctx->frames.filling->accessUnit.isInvalid){
                        K_LOG_INFO("StreamContext, invalidating frame (invalid size of 'Access unit delimiter').\n");
                        ctx->frames.filling->accessUnit.isInvalid = 1;
                    }
                } else {
                    const int primary_pic_type = (ctx->frames.filling->buff.ptr[ctx->frames.filling->buff.use - ctx->frames.fillingNalSz + 5] & 0xE0) >> 5; //u(3)
                    if(0 != VideoFrame_setAccessUnitDelimiterFound(ctx->frames.filling, primary_pic_type)){
                        K_LOG_ERROR("StreamContext, VideoFrame_setAccessUnitDelimiterFound failed.\n");
                    }
                }
            }
        }
    }
    //start new frame after copying data (keep curNal)
    if(startNewFrame && keepCurNalInCurFrame){
        K_ASSERT(isNalTypeSet)
        StreamContext_cnsmBuffNALOpenNewFilling_(ctx, plyr, flushOldersIfIsIndependent, nalType, keepCurNalInCurFrame, &filledAddedAfter);
    }
    //notify consumed
    if(filledAddedBefore || filledAddedAfter){
        //decoder
        if(ctx->dec.fd >= 0){
            //auto-start at first NAL arrival
            if(!ctx->dec.src.isExplicitON){
                if(0 != Buffers_start(&ctx->dec.src, ctx->dec.fd)){
                    K_LOG_ERROR("StreamContext, Buffers_start failed to '%s'.\n", ctx->cfg.path);
                } else {
                    K_LOG_VERBOSE("StreamContext(%lld), src-started by frame arrival '%s'.\n", (long long)ctx, ctx->cfg.path);
                }
            }
            //feed (if running and buffers are not queued yet)
            if(ctx->dec.src.isImplicitON){
                StreamContext_cnsmFrameOportunity_(ctx, plyr);
            }
            //update poll (first filled-frame is available)
            if(filledWasEmpty){
                StreamContext_updatePollMask_(ctx, plyr);
            }
        }
    }
}
    
void StreamContext_cnsmBuffNAL_(STStreamContext* ctx, struct STPlayer_* plyr, const int flushOldersIfIsIndependent){
    const unsigned char* bStart = (const unsigned char*)&ctx->buff.buff[ctx->buff.buffCsmd];
    const unsigned char* bAfterEnd = (const unsigned char*)&ctx->buff.buff[ctx->buff.buffUse];
    const unsigned char* b = bStart;
    const unsigned char* bChunkStart = bStart;
    unsigned char hdr[4] = { 0x00, 0x00, 0x00, 0x01 };
    //
    while(b < bAfterEnd){
        if(*b == 0x00){
            ctx->buff.nal.zeroesSeqAccum++;
        } else {
            //analyze end-of-header
            if(*b == 0x01 && ctx->buff.nal.zeroesSeqAccum >= 3){
                plyr->stats.curSec.src.nals.started++;
                //process data-chunk from before this header
                if(ctx->frames.filling != NULL && ctx->frames.filling->buff.use > 0){
                    const int isEndOfNAL = 1;
                    const int curChunkSz = (const int)((b + 1) - bChunkStart); //(b + 1) because we are still in last-byte-of-header
                    //
                    plyr->stats.curSec.src.nals.completed++;
                    //
                    if(curChunkSz < sizeof(hdr)){
                        //partial-header was already added to the frame, remove it
                        const int toRemoveSz = (sizeof(hdr) - curChunkSz);
                        //K_LOG_INFO("StreamContext, %d bytes of partial-nal-header removed from current frame.\n", toRemoveSz);
                        K_ASSERT(ctx->frames.filling->buff.use >= ctx->frames.fillingNalSz)
                        K_ASSERT(ctx->frames.fillingNalSz >= toRemoveSz)
                        //K_LOG_INFO("StreamContext, removed %d of %d bytes from previous NAL and closing it.\n", toRemoveSz, ctx->frames.fillingNalSz);
                        ctx->frames.filling->buff.use -= toRemoveSz;
                        ctx->frames.fillingNalSz -= toRemoveSz;
                        //analyze current frame
                        StreamContext_cnsmBuffNALChunk_(ctx, plyr, flushOldersIfIsIndependent, NULL, 0, isEndOfNAL);
                    } else {
                        //process chunk before the current header and analyze current frame
                        //K_LOG_INFO("StreamContext, adding %d (to %d bytes) to previous NAL and closing it.\n", curChunkSz - sizeof(hdr), ctx->frames.fillingNalSz + (curChunkSz - sizeof(hdr)));
                        StreamContext_cnsmBuffNALChunk_(ctx, plyr, flushOldersIfIsIndependent, bChunkStart, curChunkSz - sizeof(hdr), isEndOfNAL);
                    }
                    //
                    if(ctx->net.unitsRcvd == 0){
                        ctx->net.msToFirstUnit = ctx->net.msSinceStart;
                        if(ctx->net.msToFirstUnit > 1000){
                            K_LOG_INFO("StreamContext_tick, %u ms to receive first stream-unit: '%s'.\n", ctx->net.msToFirstUnit, ctx->cfg.path);
                        } else {
                            K_LOG_VERBOSE("StreamContext_tick, %u ms to receive first stream-unit: '%s'.\n", ctx->net.msToFirstUnit, ctx->cfg.path);
                        }
                    }
                    ctx->net.unitsRcvd++;
                }
                //start new frame (if necesary)
                if(ctx->frames.filling == NULL){
                    //create chunk
                    if(0 != VideoFrames_pullFrameForFill(&ctx->frames.reusable, &ctx->frames.filling)){
                        K_LOG_INFO("StreamContext, VideoFrames_pullFrameForFill failed.\n");
                        ctx->frames.filling = NULL;
                    } else {
                        //initial state
                        //ctx->frames.filling->state.iSeq
                        gettimeofday(&ctx->frames.filling->state.times.arrival.start, NULL);
                        gettimeofday(&ctx->frames.filling->state.times.arrival.end, NULL);
                        gettimeofday(&ctx->frames.filling->state.times.proc.start, NULL);
                        gettimeofday(&ctx->frames.filling->state.times.proc.end, NULL);
                    }
                }
                //copy current header
                if(ctx->frames.filling != NULL){
                    //copy 4 header bytes
                    if(0 != VideoFrame_copy(ctx->frames.filling, hdr, sizeof(hdr))){
                        K_LOG_ERROR("VideoFrame_copy failed.\n");
                        VideoFrame_release(ctx->frames.filling);
                        free(ctx->frames.filling);
                        ctx->frames.filling = NULL;
                        ctx->frames.fillingNalSz = 0;
                    } else {
                        ctx->frames.fillingNalSz = sizeof(hdr);
                    }
                }
                //start-of-next-chunk
                bChunkStart = b + 1;
            }
            //reset zeroes-zeq
            ctx->buff.nal.zeroesSeqAccum = 0;
        }
        b++;
    }
    //process last (unconsumed) chunk
    if(bChunkStart < bAfterEnd && ctx->frames.filling != NULL){
        const int isEndOfNAL = 0;
        const int sztoCpy = (int)(bAfterEnd - bChunkStart);
        StreamContext_cnsmBuffNALChunk_(ctx, plyr, flushOldersIfIsIndependent, bChunkStart, sztoCpy, isEndOfNAL);
    }
}

void StreamContext_pollCallbackDevice_(STStreamContext* ctx, struct STPlayer_* plyr, int revents){
    K_LOG_VERBOSE("Device, poll-event(%d):%s%s%s%s%s%s.\n", revents, (revents & POLLERR) ? " errors" : "", (revents & POLLPRI) ? " events": "", (revents & (POLLOUT | POLLWRNORM)) ? " src-hungry": "", (revents & (POLLIN | POLLRDNORM)) ? " dst-populated": "", (revents & (POLLERR | POLLPRI | POLLOUT | POLLWRNORM | POLLIN | POLLRDNORM)) == 0 ? "none" : "", (revents & ~(POLLERR | POLLPRI | POLLOUT | POLLWRNORM | POLLIN | POLLRDNORM)) != 0 ? "others" : "");
    //error
    if((revents & (POLLERR))){
        K_LOG_ERROR("Device, poll-event: error.\n");
        if(0 != StreamContext_close(ctx, plyr)){
            K_LOG_ERROR("StreamContext_close failed: '%s'.\n", ctx->cfg.path);
        }
        ctx->dec.isWaitingForIDRFrame = 1;
        ctx->dec.msToReopen = (plyr->cfg.decoderWaitRecopenSecs <= 0 ? 1 : plyr->cfg.decoderWaitRecopenSecs) * 1000;
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
    //event
    if((revents & (POLLPRI))){
        K_LOG_VERBOSE("Device, poll-event: event.\n");
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
                    const STErrCode* err = _getErrCode(errno);
                    if(err == NULL){
                        K_LOG_ERROR("StreamContext, VIDIOC_DQEVENT returned errno(%d).\n", errno);
                    } else {
                        K_LOG_ERROR("StreamContext, VIDIOC_DQEVENT returned '%s'.\n", err->str);
                    }
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
                            K_LOG_VERBOSE("StreamContext, event(V4L2_EVENT_RESOLUTION_CHANGE).\n");
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
                            //
                            if((src_change->changes & V4L2_EVENT_SRC_CH_RESOLUTION)){
                                K_LOG_VERBOSE("StreamContext, event(V4L2_EVENT_SOURCE_CHANGE) change(CH_RESOLUTION).\n");
                                src_change->changes &= ~V4L2_EVENT_SRC_CH_RESOLUTION;
                            } else {
                                K_LOG_VERBOSE("StreamContext, event(V4L2_EVENT_SOURCE_CHANGE).\n");
                            }
                            if(src_change->changes){
                                K_LOG_VERBOSE("                 change: UNKNOWN(%u).\n", src_change->changes);
                            }
                        }
                        break;
#                   endif
                    case V4L2_EVENT_EOS: //last frame has been decoded
                        K_LOG_INFO("StreamContext, event(V4L2_EVENT_EOS).\n");
                        break;
                    default:
                        K_LOG_INFO("StreamContext, event(%d, UNSUPPORTED BY THIS CODE).\n", ev.type);
                        break;
                }
                //
                if(ev.pending > 0){
                    K_LOG_INFO("StreamContext, %d pending after this one.\n", ev.pending);
                }
                //apply resolution change inmediatly
                if(isResolutionChangeEvent){
                    ctx->dec.dst.isImplicitON = 0; //resolution-change events internally stops the dst-side of the devie
                    //remove current buffers
                    if(ctx->dec.dst.isExplicitON){
                        if(0 != StreamContext_stopAndCleanupBuffs(ctx, &ctx->dec.dst, ctx->dec.fd)){
                            K_LOG_WARN("StreamContext_stopAndCleanupBuffs(dst) failed: '%s'.\n", ctx->cfg.device);
                        } else {
                            K_LOG_INFO("StreamContext, dst uninited: '%s'.\n", ctx->cfg.device);
                        }
                    }
                    //create new buffers
                    if(0 != StreamContext_initAndStartDst(ctx, plyr)){
                        K_LOG_ERROR("StreamContext_initAndStartDst(dst, %d buffers) failed: '%s'.\n", ctx->dec.dst.sz, ctx->cfg.device);
                    } else {
                        K_LOG_VERBOSE("StreamContext, dst inited and started (%d buffers): '%s'.\n", ctx->dec.dst.sz, ctx->cfg.device);
                    }
                }
                knownPends = ev.pending;
            }
        }
    }
    //src is ready for new input
    if((revents & (POLLOUT | POLLWRNORM))){
        K_LOG_VERBOSE("StreamContext(%lld), poll-event: src-hungry.\n", (long long)ctx);
        StreamContext_cnsmFrameOportunity_(ctx, plyr);
    }
    //dst has new output ready
    if((revents & (POLLIN | POLLRDNORM))){
        K_LOG_VERBOSE("StreamContext, poll-event: dst-populated.\n");
        {
            STBuffer* buff = NULL;
            struct timeval timestamp;
            memset(&timestamp, 0, sizeof(timestamp));
            if(0 != Buffers_dequeue(&ctx->dec.dst, ctx->dec.fd, &buff, &timestamp)){
                buff = NULL;
            } else {
                int framesSkippedByDecoderCount = 0;
                unsigned long frameSeqIdx = 0; STVideoFrameState frameState;
                //
                ctx->dec.framesOutSinceOpen++;
                if(ctx->dec.framesOutSinceOpen == 1){
                    ctx->dec.msFirstFrameOut = ctx->dec.msOpen;
                    if(ctx->dec.msFirstFrameOut >= 1000){
                        K_LOG_INFO("StreamContext, %ums + %ums to produce first decoded frame.\n", ctx->dec.msFirstFrameFed, (ctx->dec.msFirstFrameOut - ctx->dec.msFirstFrameFed));
                    } else {
                        K_LOG_VERBOSE("StreamContext, %ums + %ums to produce first decoded frame.\n", ctx->dec.msFirstFrameFed, (ctx->dec.msFirstFrameOut - ctx->dec.msFirstFrameFed));
                    }
                }
                //
                VideoFrameState_init(&frameState);
                VideoFrameState_timestampToSeqIdx(&timestamp, &frameSeqIdx);
                VideoFrameStates_getStateCloningAndRemoveOlder(&ctx->dec.frames.fed, frameSeqIdx, &frameState, &framesSkippedByDecoderCount);
                if(frameState.iSeq == frameSeqIdx){
                    gettimeofday(&frameState.times.proc.end, NULL);
                    long msToArrive = msBetweenTimevals(&frameState.times.proc.start, &frameState.times.proc.end);
                    plyr->stats.curSec.dec.got.msSum += msToArrive;
                    if(plyr->stats.curSec.dec.got.count == 0){
                        plyr->stats.curSec.dec.got.msMin = msToArrive;
                        plyr->stats.curSec.dec.got.msMax = msToArrive;
                    } else {
                        if(plyr->stats.curSec.dec.got.msMin > msToArrive) plyr->stats.curSec.dec.got.msMin = msToArrive;
                        if(plyr->stats.curSec.dec.got.msMax < msToArrive) plyr->stats.curSec.dec.got.msMax = msToArrive;
                    }
                    K_LOG_VERBOSE("StreamContext, frame(#%d) output obtained (%dms inside device).\n", (frameSeqIdx + 1), msToArrive);
                } else {
                    //stats
                    plyr->stats.curSec.dec.got.count++;
                    K_LOG_VERBOSE("StreamContext, frame(#%d) output obtained (no state-fed found).\n", (frameSeqIdx + 1));
                }
                plyr->stats.curSec.dec.got.count++;
                plyr->stats.curSec.dec.got.skipped += framesSkippedByDecoderCount;
                if(framesSkippedByDecoderCount > 0){
                    K_LOG_WARN("StreamContext, decoder skipped %d frames fed (when obtaining frame #%u).\n", framesSkippedByDecoderCount, frameSeqIdx + 1);
                }
                //flag framebuffers-grps where this frame will be used
                {
                    int i; for(i = 0; i < plyr->fbs.grps.use; i++){
                        STFramebuffsGrp* grp = &plyr->fbs.grps.arr[i];
                        if(grp->pixFmt == ctx->drawPlan.lastPixelformat){
                            if(0 == FramebuffsGrp_layoutFindStreamId(grp, ctx->streamId)){
                                //flag as dirty
                                grp->isSynced = 0;
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
                            K_LOG_ERROR("StreamContext, dst-buff could not be queued.\n");
                            buffFnd = NULL;
                        } else {
                            //
                        }
                    }
                    //add current bufer if necesary
                    if(buffFnd == NULL && buff != NULL){
                        //keep last as a clone
                        //ToDo: clone or not clone (memory consume)
                        /*if(0 != Buffers_keepLastAsClone(&ctx->dec.dst, buff)){
                            K_LOG_ERROR("StreamContext, frame(#%d) could not be cloned to 'last'.\n", (frameSeqIdx + 1));
                        }*/
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
                            K_LOG_ERROR("StreamContext, dst-buff could not be queued.\n");
                            buffFnd = NULL;
                        } else {
                            //
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
}

int StreamContext_shutdownStartByFileClosed_(STStreamContext* ctx, struct STPlayer_* plyr, const char* reason){
    int r = 0;
    //shutdown permanently (file will not be opened again)
    const int isPermanent = 1;
    if(ctx->shuttingDown.isActive){
        ctx->shuttingDown.isPermanent = isPermanent;
        //K_LOG_INFO("StreamContext, shutdownStartByFileClosed_ current shutdown flagged (at '%s').\n", reason);
    } else {
        if(0 != StreamContext_shutdownStart(ctx, plyr, isPermanent)){
            K_LOG_ERROR("StreamContext, shutdownStartByFileClosed_ failed (at '%s').\n", reason);
        } else {
            //K_LOG_INFO("StreamContext, shutdownStartByFileClosed_ shutdown started (at '%s').\n", reason);
        }
    }
    //close file
    if(ctx->file.fd > 0){
        Player_pollAutoRemove(plyr, ENPlayerPollFdType_SrcFile, ctx, ctx->file.fd);
        close(ctx->file.fd);
        ctx->file.fd = -1;
    }
    ctx->file.msToReconnect = (plyr->cfg.connWaitReconnSecs > 0 ? plyr->cfg.connWaitReconnSecs : 1) * 1000;
    return r;
}

void StreamContext_pollCallbackFile_(STStreamContext* ctx, struct STPlayer_* plyr, int revents){
    //error
    if((revents & (POLLERR))){
        const char* reason = "file poll-err";
        if(0 != StreamContext_shutdownStartByFileClosed_(ctx, plyr, reason)){
            K_LOG_ERROR("StreamContext, StreamContext_shutdownStart failed (at '%s').\n", reason);
        }
    } else {
        if((revents & POLLIN)){
            int rcvd = 1;
            unsigned long lastNALPushedSeq = ctx->frames.filled.iSeqPushNext; //to detect completed nals parsed
            const unsigned long curScreenRefreshSeq = plyr->anim.tickSeq;
            //read untill no data is returned or explicit-stop-reading
            while(
                  !ctx->flushing.isActive //not flushing
                  && lastNALPushedSeq == ctx->frames.filled.iSeqPushNext //rading same NAL
                  && rcvd > 0 //data read
                  )
            {
                rcvd = 0;
                //empty buffer if fully consumed (lock is not necesary)
                if(ctx->buff.buffCsmd >= ctx->buff.buffUse){
                    ctx->buff.buffCsmd = 0;
                    ctx->buff.buffUse = 0;
                }
                //produce
                if(ctx->buff.buffUse < ctx->buff.buffSz){
                    //produce (unlocked)
                    rcvd = (int)read(ctx->file.fd, &ctx->buff.buff[ctx->buff.buffUse], (ctx->buff.buffSz - ctx->buff.buffUse));
                    if(rcvd > 0){
                        K_LOG_VERBOSE("File, %d/%d read.\n", rcvd, (ctx->buff.buffSz - ctx->buff.buffUse));
                        ctx->buff.buffUse += rcvd;
                        //read body (NALs)
                        StreamContext_cnsmBuffNAL_(ctx, plyr, 0 /*flushOldersIfIsIndependent*/);
                        //mark as fully consumed
                        ctx->file.msWithoutRead = 0;
                        ctx->buff.buffCsmd = ctx->buff.buffUse;
                        //new frame arrived, wait for screen refresh
                        if(lastNALPushedSeq != ctx->frames.filled.iSeqPushNext){
                            //wait untill screen refresh
                            ctx->buff.screenRefreshSeqBlocking = curScreenRefreshSeq;
                            if(0 != Player_pollUpdate(plyr, ENPlayerPollFdType_SrcFile, ctx, ctx->file.fd, 0, NULL)){ //nothing
                                const char* reason = "poll-update-failed";
                                if(0 != StreamContext_shutdownStartByFileClosed_(ctx, plyr, reason)){
                                    K_LOG_ERROR("StreamContext, StreamContext_shutdownStart failed (at '%s').\n", reason);
                                }
                            }
                        }
                    } else if(rcvd != 0){ //zero = socket propperly shuteddown
                        if(errno == EAGAIN || errno == EWOULDBLOCK){
                            //non-blocking
                        } else {
                            const char* reason = "file-read-failed";
                            if(0 != StreamContext_shutdownStartByFileClosed_(ctx, plyr, reason)){
                                K_LOG_ERROR("StreamContext, StreamContext_shutdownStart failed (at '%s').\n", reason);
                            }
                        }
                    }
                }
            }
        }
    }
}

void StreamContext_pollCallbackSocket_(STStreamContext* ctx, struct STPlayer_* plyr, int revents){
    //error
    if((revents & (POLLERR))){
        K_LOG_ERROR("StreamContext, poll-err-flag active at socket '%s:%d'.\n", ctx->cfg.server, ctx->cfg.port);
        Player_pollAutoRemove(plyr, ENPlayerPollFdType_SrcSocket, ctx, ctx->net.socket);
        close(ctx->net.socket);
        ctx->net.socket = 0;
        ctx->net.msToReconnect = (plyr->cfg.connWaitReconnSecs > 0 ? plyr->cfg.connWaitReconnSecs : 1) * 1000;
    } else {
        //write
        if((revents & POLLOUT)){
            //send
            if(ctx->net.req.payCsmd < ctx->net.req.payUse){
                const int sent = (int)send(ctx->net.socket, &ctx->net.req.pay[ctx->net.req.payCsmd], (ctx->net.req.payUse - ctx->net.req.payCsmd), 0);
                if(sent > 0){
                    if(ctx->net.bytesSent == 0){
                        ctx->net.msToConnect = ctx->net.msSinceStart;
                        if(ctx->net.msToConnect > 1000){
                            K_LOG_INFO("StreamContext_tick, %u ms to connect: '%s'.\n", ctx->net.msToConnect, ctx->cfg.path);
                        } else {
                            K_LOG_VERBOSE("StreamContext_tick, %u ms to connect: '%s'.\n", ctx->net.msToConnect, ctx->cfg.path);
                        }
                    }
                    ctx->net.bytesSent += sent;
                    ctx->net.msWithoutSend = 0;
                    ctx->net.req.payCsmd += sent;
                    if(ctx->net.req.payUse == ctx->net.req.payCsmd){
                        K_LOG_VERBOSE("StreamContext, request sent (%d bytes) to '%s:%d'.\n", ctx->net.req.payCsmd, ctx->cfg.server, ctx->cfg.port);
                        K_LOG_VERBOSE("StreamContext, -->\n%s\n<--\n", ctx->net.req.pay);
                        //stop writting, start reading-only
                        if(0 != Player_pollUpdate(plyr, ENPlayerPollFdType_SrcSocket, ctx, ctx->net.socket, POLLIN, NULL)){ //read
                            K_LOG_ERROR("StreamContext, poll-update-failed to '%s:%d'.\n", ctx->cfg.server, ctx->cfg.port);
                            Player_pollAutoRemove(plyr, ENPlayerPollFdType_SrcSocket, ctx, ctx->net.socket);
                            close(ctx->net.socket);
                            ctx->net.socket = 0;
                            ctx->net.msToReconnect = (plyr->cfg.connWaitReconnSecs > 0 ? plyr->cfg.connWaitReconnSecs : 1) * 1000;
                        }
                    }
                } else if(sent != 0){ //zero = socket propperly shuteddown
                    if(errno == EAGAIN || errno == EWOULDBLOCK){
                        //non-blocking
                    } else {
                        K_LOG_ERROR("StreamContext, send failed to '%s:%d'.\n", ctx->cfg.server, ctx->cfg.port);
                        Player_pollAutoRemove(plyr, ENPlayerPollFdType_SrcSocket, ctx, ctx->net.socket);
                        close(ctx->net.socket);
                        ctx->net.socket = 0;
                        ctx->net.msToReconnect = (plyr->cfg.connWaitReconnSecs > 0 ? plyr->cfg.connWaitReconnSecs : 1) * 1000;
                    }
                }
            }
        }
        //read (ToDo: read untill no more data is available)
        if((revents & POLLIN)){
            int rcvd = 0, bytesRcvdBefore = 0;
            unsigned long lastNALPushedSeq = ctx->frames.filled.iSeqPushNext; //to detect completed nals parsed
            //recv untill no data is returned or explicit-stop-reading
            do {
                rcvd = 0;
                //empty buffer if fully consumed (lock is not necesary)
                if(ctx->buff.buffCsmd >= ctx->buff.buffUse){
                    ctx->buff.buffCsmd = 0;
                    ctx->buff.buffUse = 0;
                }
                //produce
                if(ctx->buff.buffUse < ctx->buff.buffSz){
                    //produce (unlocked)
                    bytesRcvdBefore = ctx->net.bytesRcvd;
                    rcvd = (int)recv(ctx->net.socket, &ctx->buff.buff[ctx->buff.buffUse], (ctx->buff.buffSz - ctx->buff.buffUse), 0);
                    if(rcvd > 0){
                        K_LOG_VERBOSE("Net, %d/%d revd.\n", rcvd, (ctx->buff.buffSz - ctx->buff.buffUse));
                        if(ctx->net.bytesRcvd == 0){
                            ctx->net.msToRespStart = ctx->net.msSinceStart;
                            if(ctx->net.msToRespStart > 1000){
                                K_LOG_INFO("StreamContext_tick, %u ms to start receiving response: '%s'.\n", ctx->net.msToRespStart, ctx->cfg.path);
                            } else {
                                K_LOG_VERBOSE("StreamContext_tick, %u ms to start receiving response: '%s'.\n", ctx->net.msToRespStart, ctx->cfg.path);
                            }
                        }
                        ctx->net.bytesRcvd += rcvd;
                        ctx->buff.buffUse += rcvd;
                        //read header
                        if(!ctx->net.resp.headerEnded){
                            StreamContext_cnsmRespHttpHeader_(ctx);
                            if(ctx->net.resp.headerEnded){
                                ctx->net.msToRespHead = ctx->net.msSinceStart;
                                if(ctx->net.msToRespHead > 1000){
                                    K_LOG_INFO("StreamContext_tick, %u ms to receive response header: '%s'.\n", ctx->net.msToRespHead, ctx->cfg.path);
                                } else {
                                    K_LOG_VERBOSE("StreamContext_tick, %u ms to receive response header: '%s'.\n", ctx->net.msToRespHead, ctx->cfg.path);
                                }
                            }
                        }
                        //read body (NALs)
                        if(ctx->net.resp.headerEnded && ctx->buff.buffCsmd < ctx->buff.buffUse){
                            if(bytesRcvdBefore < ctx->net.resp.headerSz && ctx->net.bytesRcvd >= ctx->net.resp.headerSz){
                                ctx->net.msToRespBody = ctx->net.msSinceStart;
                                if(ctx->net.msToRespBody > 1000){
                                    K_LOG_INFO("StreamContext_tick, %u ms to start receiving body: '%s'.\n", ctx->net.msToRespBody, ctx->cfg.path);
                                } else {
                                    K_LOG_VERBOSE("StreamContext_tick, %u ms to start receiving body: '%s'.\n", ctx->net.msToRespBody, ctx->cfg.path);
                                }
                            }
                            StreamContext_cnsmBuffNAL_(ctx, plyr, 1/*flushOldersIfIsIndependent*/);
                        }
                        //mark as fully consumed
                        ctx->net.msWithoutRecv = 0;
                        ctx->buff.buffCsmd = ctx->buff.buffUse;
                    } else if(rcvd != 0){ //zero = socket propperly shuteddown
                        if(errno == EAGAIN || errno == EWOULDBLOCK){
                            //non-blocking
                        } else {
                            K_LOG_ERROR("StreamContext, recv failed to '%s:%d'.\n", ctx->cfg.server, ctx->cfg.port);
                            Player_pollAutoRemove(plyr, ENPlayerPollFdType_SrcSocket, ctx, ctx->net.socket);
                            close(ctx->net.socket);
                            ctx->net.socket = 0;
                            ctx->net.msToReconnect = (plyr->cfg.connWaitReconnSecs > 0 ? plyr->cfg.connWaitReconnSecs : 1) * 1000;
                        }
                    }
                }
            } while(rcvd > 0 && lastNALPushedSeq == ctx->frames.filled.iSeqPushNext);
        }
    }
}
    
void StreamContext_pollCallback(void* userParam, struct STPlayer_* plyr, const ENPlayerPollFdType type, int revents){
    STStreamContext* ctx = (STStreamContext*)userParam;
    switch(type){
        case ENPlayerPollFdType_Decoder: //dec (decoder).fd
            StreamContext_pollCallbackDevice_(ctx, plyr, revents);
            break;
        case ENPlayerPollFdType_SrcFile: //net.socket
            StreamContext_pollCallbackFile_(ctx, plyr, revents);
            break;
        case ENPlayerPollFdType_SrcSocket: //net.socket
            StreamContext_pollCallbackSocket_(ctx, plyr, revents);
            break;
        default:
            break;
    }
}

int StreamContext_isSame(STStreamContext* ctx, const char* device, const char* server, const unsigned int port, const char* resPath, int srcPixFmt /*V4L2_PIX_FMT_H264*/, int dstPixFmt /*V4L2_PIX_FMT_RGB565*/){
    int r = 0;
    if(device != ctx->cfg.device && (device == NULL || ctx->cfg.device == NULL || strcmp(device, ctx->cfg.device) != 0)){
        r = -1;
    } else if(server != ctx->cfg.server && (server == NULL || ctx->cfg.server == NULL || strcmp(server, ctx->cfg.server) != 0)){
        r = -1;
    } else if(port != ctx->cfg.port){
        r = -1;
    } else if(resPath != ctx->cfg.path && (resPath == NULL || ctx->cfg.path == NULL || strcmp(resPath, ctx->cfg.path) != 0)){
        r = -1;
    } else if(srcPixFmt != ctx->cfg.srcPixFmt){
        r = -1;
    } else if(dstPixFmt != ctx->cfg.dstPixFmt){
        r = -1;
    }
    return r;
}

int StreamContext_open(STStreamContext* ctx, struct STPlayer_* plyr, const char* device, const char* server, const unsigned int port, const int keepAlive, const char* resPath, int srcPixFmt /*V4L2_PIX_FMT_H264*/, int buffersAmmount, int planesPerBuffer, int sizePerPlane, int dstPixFmt /*V4L2_PIX_FMT_*/, const int connTimeoutSecs, const int decoderTimeoutSecs, const unsigned long framesSkip, const unsigned long framesFeedMax){
    int r = -1;
    //
    if(device == NULL || device[0] == '\0'){
        K_LOG_ERROR("StreamContext_open, device-param is empty.\n");
    } else if(resPath == NULL || resPath[0] == '\0'){
        K_LOG_ERROR("StreamContext_open, resPath-param is empty.\n");
#   ifdef K_USE_MPLANE
    } else if(0 != Buffers_setNameAndType(&ctx->dec.src, "src", V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)){
        K_LOG_ERROR("StreamContext_open, Buffers_setNameAndType failed.\n");
#   else
    } else if(0 != Buffers_setNameAndType(&ctx->dec.src, "src", V4L2_BUF_TYPE_VIDEO_OUTPUT)){
        K_LOG_ERROR("StreamContext_open, Buffers_setNameAndType failed.\n");
#   endif
#   ifdef K_USE_MPLANE
    } else if(0 != Buffers_setNameAndType(&ctx->dec.dst, "dst", V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)){
        K_LOG_ERROR("StreamContext_open, Buffers_setNameAndType failed.\n");
#   else
    } else if(0 != Buffers_setNameAndType(&ctx->dec.dst, "dst", V4L2_BUF_TYPE_VIDEO_CAPTURE)){
        K_LOG_ERROR("StreamContext_open, Buffers_setNameAndType failed.\n");
#   endif
    } else if(ctx->shuttingDown.isActive && ctx->shuttingDown.isPermanent){
        K_LOG_ERROR("StreamContext_open, context was previously shutted-down permnently (program logic error).\n");
        K_ASSERT(!(ctx->shuttingDown.isActive && ctx->shuttingDown.isPermanent));
    } else {
        K_LOG_VERBOSE("StreamContext_open, opening device: '%s'...\n", resPath);
        int fd = v4l2_open(device, O_RDWR | O_NONBLOCK);
        if(fd < 0){
            K_LOG_ERROR("StreamContext_open, device failed to open: '%s'.\n", resPath);
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
                K_LOG_ERROR("v4lDevice_queryCaps failed: '%s'.\n", resPath);
            } else if(0 != Buffers_queryFmts(&ctx->dec.src, fd, srcPixFmt, &srcPixFmtWasFound, (printSrcFmt != NULL ? 1 : 0))){
                K_LOG_ERROR("Buffers_queryFmts(src) failed: '%s'.\n", resPath);
            } else if(!srcPixFmtWasFound){
                K_LOG_ERROR("Buffers_queryFmts src-fmt unsupported: '%s'.\n", resPath);
            } else if(0 != Buffers_setFmt(&ctx->dec.src, fd, srcPixFmt, planesPerBuffer, sizePerPlane, 0 /*getCompositionRect*/, (printSrcFmt != NULL ? 1 : 0))){
                K_LOG_ERROR("Buffers_setFmt failed: '%s'.\n", resPath);
            } else if(0 != Buffers_queryFmts(&ctx->dec.dst, fd, dstPixFmt, &dstPixFmtWasFound, (printSrcFmt != NULL ? 1 : 0))){
                K_LOG_ERROR("Buffers_queryFmts(dst) failed: '%s'.\n", resPath);
            } else if(!dstPixFmtWasFound){
                K_LOG_ERROR("Buffers_queryFmts dst-fmt('%c%c%c%c') unsupported: '%s'.\n", dstPixFmtChars[0], dstPixFmtChars[1], dstPixFmtChars[2], dstPixFmtChars[3], resPath);
            } else if(0 != StreamContext_initAndPrepareSrc(ctx, fd, buffersAmmount, (printSrcFmt != NULL ? 1 : 0))){
                K_LOG_ERROR("StreamContext_initAndPrepareSrc(%d) failed: '%s'.\n", buffersAmmount, resPath);
            } else {
                if(0 != StreamContext_eventsSubscribe(ctx, fd)){
                    K_LOG_ERROR("StreamContext_eventsSubscribe failed to '%s'.\n", resPath);
                } else {
                    if(0 != Player_pollAdd(plyr, ENPlayerPollFdType_Decoder, StreamContext_pollCallback, ctx, fd, StreamContext_getPollEventsMask(ctx))){ //write
                        K_LOG_ERROR("Player_pollAdd poll-add-failed to '%s'.\n", resPath);
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
                            ctx->cfg.keepAlive          = keepAlive;
                            //
                            ctx->cfg.srcPixFmt          = srcPixFmt; //V4L2_PIX_FMT_H264
                            ctx->cfg.buffersAmmount     = buffersAmmount;
                            ctx->cfg.planesPerBuffer    = planesPerBuffer;
                            ctx->cfg.sizePerPlane       = sizePerPlane;
                            ctx->cfg.dstPixFmt          = dstPixFmt; //V4L2_PIX_FMT_*
                            //
                            ctx->cfg.connTimeoutSecs    = connTimeoutSecs;
                            ctx->cfg.decoderTimeoutSecs = decoderTimeoutSecs;
                            //
                            ctx->cfg.framesSkip         = framesSkip;
                            ctx->cfg.framesFeedMax      = framesFeedMax;
                            K_LOG_VERBOSE("StreamContext_open framesSkip(%u), framesFeedMax(%u).\n", ctx->cfg.framesSkip, ctx->cfg.framesFeedMax);
                        }
                        //dec (decoder)
                        {
                            if(ctx->dec.fd >= 0){
                                v4l2_close(ctx->dec.fd);
                                ctx->dec.fd = -1;
                            }
                            ctx->dec.fd = fd; fd = -1; //consume
                            {
                                ctx->dec.msOpen = 0;
                                ctx->dec.msFirstFrameFed = 0;
                                ctx->dec.framesInSinceOpen = 0;
                                ctx->dec.framesOutSinceOpen = 0;
                                //
                                ctx->dec.msWithoutFeedFrame = 0;
                                ctx->dec.isWaitingForIDRFrame = 1;
                                //
                                memset(&ctx->flushing, 0, sizeof(ctx->flushing));
                                memset(&ctx->shuttingDown, 0, sizeof(ctx->shuttingDown));
                                K_ASSERT(!ctx->flushing.isActive)
                                K_ASSERT(!ctx->shuttingDown.isActive)
                                //
                                K_ASSERT(!ctx->dec.src.isExplicitON && !ctx->dec.src.isImplicitON) //should be off
                                K_ASSERT(ctx->dec.src.enqueuedCount == 0) //should be zero
                                K_ASSERT(!ctx->dec.dst.isExplicitON && !ctx->dec.dst.isImplicitON) //should be off
                                K_ASSERT(ctx->dec.dst.enqueuedCount == 0) //should be zero
                            }
                            if(ctx->dec.frames.fed.use > 0){
                                K_LOG_VERBOSE("StreamContext, %d fed-states discarded at open.\n", ctx->dec.frames.fed.use);
                                VideoFrameStates_empty(&ctx->dec.frames.fed);
                            }
                        }
                        //success
                        r = 0;
                    }
                    //revert
                    if(r != 0 && fd >= 0){
                        if(0 != StreamContext_eventsUnsubscribe(ctx, fd)){
                            K_LOG_WARN("StreamContext_eventsUnsubscribe failed.\n");
                        }
                    }
                }
                //revert
                if(r != 0 && fd >= 0){
                    if(0 != StreamContext_stopAndCleanupBuffs(ctx, &ctx->dec.src, fd)){
                        K_LOG_WARN("StreamContext_stopAndCleanupBuffs(src) failed.\n");
                    }
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
    if(ctx->dec.fd >= 0){
        if(0 != StreamContext_stopAndCleanupBuffs(ctx, &ctx->dec.dst, ctx->dec.fd)){
            K_LOG_WARN("StreamContext_stopAndCleanupBuffs(dst) failed.\n");
        }
        if(0 != StreamContext_stopAndCleanupBuffs(ctx, &ctx->dec.src, ctx->dec.fd)){
            K_LOG_WARN("StreamContext_stopAndCleanupBuffs(src) failed.\n");
        }
        if(0 != StreamContext_eventsUnsubscribe(ctx, ctx->dec.fd)){
            K_LOG_ERROR("StreamContext, unsubscribe failed.\n");
        }
        if(0 != Player_pollAutoRemove(plyr, ENPlayerPollFdType_Decoder, ctx, ctx->dec.fd)){
            K_LOG_ERROR("StreamContext, Player_pollAutoRemove failed.\n");
        }
        v4l2_close(ctx->dec.fd);
        ctx->dec.fd = -1;
    }
    //reset (just in case)
    {
        ctx->dec.msOpen = 0;
        ctx->dec.msFirstFrameFed = 0;
        ctx->dec.framesInSinceOpen = 0;
        ctx->dec.framesOutSinceOpen = 0;
        //
        ctx->dec.src.isExplicitON = 0;
        ctx->dec.src.isImplicitON = 0;
        ctx->dec.src.enqueuedCount = 0;
        //
        ctx->dec.dst.isExplicitON = 0;
        ctx->dec.dst.isImplicitON = 0;
        ctx->dec.dst.enqueuedCount = 0;
        //
        K_ASSERT(!ctx->dec.src.isExplicitON && !ctx->dec.src.isImplicitON) //should be off
        K_ASSERT(ctx->dec.src.enqueuedCount == 0) //should be zero
        K_ASSERT(!ctx->dec.dst.isExplicitON && !ctx->dec.dst.isImplicitON) //should be off
        K_ASSERT(ctx->dec.dst.enqueuedCount == 0) //should be zero
    }
    if(ctx->dec.frames.fed.use > 0){
        K_LOG_VERBOSE("StreamContext, %d fed-states discarded at close.\n", ctx->dec.frames.fed.use);
        VideoFrameStates_empty(&ctx->dec.frames.fed);
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
        if(obj->isMmaped){
            if(obj->isOrphanable){
                int rr = v4l2_munmap(obj->dataPtr, obj->length);
                if(rr == 0){
                    K_LOG_VERBOSE("Plane munmapped errno(%d).\n", errno);
                } else {
                    K_LOG_ERROR("munmap errno(%d).\n", errno);
                }
            }
        } else {
            free(obj->dataPtr);
        }
        obj->dataPtr = NULL;
    }
    if(obj->fd >= 0){
        if(obj->isOrphanable){
            close(obj->fd);
        }
        obj->fd = -1;
    }
    obj->isOrphanable = 0;
    obj->isMmaped = 0;
    obj->used = 0;
    obj->length = 0;
    obj->bytesPerLn = 0;
    obj->memOffset = 0;
}

int Plane_clone(STPlane* obj, const STPlane* src){
    int r = -1;
    //resize plane (if posible)
    if(obj->length != src->length && !obj->isMmaped){
        K_LOG_VERBOSE("Plane, clone, resizing length.\n");
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
        K_LOG_VERBOSE("Buffer, clone, resizing planes.\n");
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
        int i; for(i = 0; i < obj->sz; i++){
            STBuffer* b = &obj->arr[i];
            Buffer_release(b);
        }
        free(obj->arr);
        obj->arr = NULL;
    }
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
                K_LOG_ERROR("unsupported buffers-type(%d).", type);
                r - 1;
                break;
        }
    }
    return r;
}


int Buffers_queryFmts(STBuffers* obj, int fd, int fmtSearch, int* dstFmtWasFound, const int print){
    int r = -1;
    if(print){
        K_LOG_INFO("--------------------------.\n");
        K_LOG_INFO("---- QUERING FORMATS  ----.\n");
        K_LOG_INFO("---- '%s'.\n", obj->name);
        K_LOG_INFO("--------------------------.\n");
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
                    K_LOG_INFO("Buffers(%s), coded format #%d: '%c%c%c%c' => '%s'.\n", obj->name, (fmt.index + 1), pixelformatBytes[0], pixelformatBytes[1], pixelformatBytes[2], pixelformatBytes[3], fmt.description);
                }
                //
                if(fmt.pixelformat == fmtSearch){
                    if(dstFmtWasFound != NULL){
                        *dstFmtWasFound = 1;
                    }
                }
                //flags
                if(print){
                    if((fmt.flags & V4L2_FMT_FLAG_COMPRESSED)){ K_LOG_INFO("                flag: V4L2_FMT_FLAG_COMPRESSED.\n"); }
                    if((fmt.flags & V4L2_FMT_FLAG_EMULATED)){ K_LOG_INFO("                flag: V4L2_FMT_FLAG_EMULATED.\n"); }
                    //V4L2_FMT_FLAG_CONTINUOUS_BYTESTREAM: the engine can receive bytestream, if not, the engine expects one H264 Access Unit per buffer.
#                   ifdef V4L2_FMT_FLAG_CONTINUOUS_BYTESTREAM //jetson-nano does not recognizes this
                    if((fmt.flags & V4L2_FMT_FLAG_CONTINUOUS_BYTESTREAM)){ K_LOG_INFO("    flag: V4L2_FMT_FLAG_CONTINUOUS_BYTESTREAM.\n"); }
#                   endif
#                   ifdef V4L2_FMT_FLAG_DYN_RESOLUTION //jetson-nano does not recognizes this
                    if((fmt.flags & V4L2_FMT_FLAG_DYN_RESOLUTION)){ K_LOG_INFO("    flag: V4L2_FMT_FLAG_DYN_RESOLUTION.\n"); }
#                   endif
#                   ifdef V4L2_FMT_FLAG_ENC_CAP_FRAME_INTERVAL //jetson-nano does not recognizes this
                    if((fmt.flags & V4L2_FMT_FLAG_ENC_CAP_FRAME_INTERVAL)){ K_LOG_INFO("    flag: V4L2_FMT_FLAG_ENC_CAP_FRAME_INTERVAL.\n"); }
#                   endif
#                   ifdef V4L2_FMT_FLAG_CSC_COLORSPACE //jetson-nano does not recognizes this
                    if((fmt.flags & V4L2_FMT_FLAG_CSC_COLORSPACE)){ K_LOG_INFO("    flag: V4L2_FMT_FLAG_CSC_COLORSPACE.\n"); }
#                   endif
#                   ifdef V4L2_FMT_FLAG_CSC_XFER_FUNC //jetson-nano does not recognizes this
                    if((fmt.flags & V4L2_FMT_FLAG_CSC_XFER_FUNC)){ K_LOG_INFO("    flag: V4L2_FMT_FLAG_CSC_XFER_FUNC.\n"); }
#                   endif
#                   ifdef V4L2_FMT_FLAG_CSC_YCBCR_ENC //jetson-nano does not recognizes this
                    if((fmt.flags & V4L2_FMT_FLAG_CSC_YCBCR_ENC)){ K_LOG_INFO("    flag: V4L2_FMT_FLAG_CSC_YCBCR_ENC.\n"); }
#                   endif
#                   ifdef V4L2_FMT_FLAG_CSC_HSV_ENC //jetson-nano does not recognizes this
                    if((fmt.flags & V4L2_FMT_FLAG_CSC_HSV_ENC)){ K_LOG_INFO("    flag: V4L2_FMT_FLAG_CSC_HSV_ENC.\n"); }
#                   endif
#                   ifdef V4L2_FMT_FLAG_CSC_QUANTIZATION
                    if((fmt.flags & V4L2_FMT_FLAG_CSC_QUANTIZATION)){ K_LOG_INFO("    flag: V4L2_FMT_FLAG_CSC_QUANTIZATION.\n"); }
#                   endif
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
                                        K_LOG_INFO("                framesize #%d: discrete, width(%u) height(%u).\n", (sz.index + 1), sz.discrete.width, sz.discrete.height);
                                        break;
                                    case V4L2_FRMSIZE_TYPE_CONTINUOUS:
                                        //this is like stepwise, but only one step is defined since steps_sizes are = 1.
                                        K_LOG_INFO("                framesize #%d: continuous, width(%u, +%u, %u) height(%u, +%u, %u).\n", (sz.index + 1), sz.stepwise.min_width, sz.stepwise.step_width, sz.stepwise.max_width, sz.stepwise.min_height, sz.stepwise.step_height, sz.stepwise.max_height);
                                        break;
                                    case V4L2_FRMSIZE_TYPE_STEPWISE:
                                        K_LOG_INFO("                framesize #%d: stepwise, width(%u, +%u, %u) height(%u, +%u, %u).\n", (sz.index + 1), sz.stepwise.min_width, sz.stepwise.step_width, sz.stepwise.max_width, sz.stepwise.min_height, sz.stepwise.step_height, sz.stepwise.max_height);
                                        break;
                                    default:
                                        K_LOG_INFO("                framesize #%d: unknown type.\n", (sz.index + 1));
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

int Buffers_setFmt(STBuffers* obj, int fd, int fmt, int planesPerBuffer, int sizePerPlane, const int getCompositionRect, const int print){
    int r = -1;
    if(print){
        K_LOG_INFO("-------------------------------.\n");
        K_LOG_INFO("---- CONFIGURING BUFFERS   ----.\n");
        K_LOG_INFO("---- '%s'.\n", obj->name);
        K_LOG_INFO("-------------------------------.\n");
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
                const STErrCode* err = _getErrCode(errno);
                if(err == NULL){
                    K_LOG_ERROR("Buffers(%s), getting src-format returned errno(%d).\n", obj->name, errno);
                } else {
                    K_LOG_ERROR("Buffers(%s), getting src-format returned '%s'.\n", obj->name, err->str);
                }
            } else {
                switch(obj->type){
#                   ifdef K_USE_MPLANE
                    case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
                    case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
                        if(print){
                            K_LOG_INFO("Buffers(%s), getting pixelformat('%c%c%c%c') width(%u) height(%u) success.\n", obj->name, pixelformatBytes[0], pixelformatBytes[1], pixelformatBytes[2], pixelformatBytes[3], obj->mp->width, obj->mp->height);
                        }
                        obj->pixelformat = obj->mp->pixelformat;
                        obj->width  = obj->mp->width;
                        obj->height = obj->mp->height;
                        {
                            int i; for( i = 0; i < obj->mp->num_planes; i++){
                                struct v4l2_plane_pix_format* pp = &obj->mp->plane_fmt[i];
                                if(print){
                                    K_LOG_INFO("    plane #%d, sizeimage(%u) bytesperline(%u).\n", (i + 1), pp->sizeimage, pp->bytesperline);
                                }
                            }
                        }
                        break;
#                   else
                    case V4L2_BUF_TYPE_VIDEO_OUTPUT:
                    case V4L2_BUF_TYPE_VIDEO_CAPTURE:
                        if(print){
                            K_LOG_INFO("Buffers(%s), getting pixelformat('%c%c%c%c') width(%u) height(%u) sizeimage(%u) bytesperline(%u) success.\n", obj->name, pixelformatBytes[0], pixelformatBytes[1], pixelformatBytes[2], pixelformatBytes[3], obj->sp->width, obj->sp->height, obj->sp->sizeimage, obj->sp->bytesperline);
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
                if(getCompositionRect){
                    if(0 != Buffers_getCompositionRect(obj, fd, &obj->composition)){
                        if(print){
                            K_LOG_INFO("Buffers(%s), getting getCompositionRect returned(%d).\n", obj->name, rr);
                        }
                        obj->composition.x = 0;
                        obj->composition.y = 0;
                        obj->composition.width = obj->width;
                        obj->composition.height = obj->height;
                        if(print){
                            K_LOG_INFO("Buffers(%s), implicit composition x(%d, +%d) y(%d, +%d).\n", obj->name, obj->composition.x, obj->composition.width, obj->composition.y, obj->composition.height);
                        }
                    } else {
                        if(print){
                            K_LOG_INFO("Buffers(%s), explicit composition x(%d, +%d) y(%d, +%d).\n", obj->name, obj->composition.x, obj->composition.width, obj->composition.y, obj->composition.height);
                        }
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
                        K_LOG_INFO("Buffers(%s), setting pixelformat('%c%c%c%c') width(%u) height(%u).\n", obj->name, pixelformatBytes[0], pixelformatBytes[1], pixelformatBytes[2], pixelformatBytes[3], obj->mp->width, obj->mp->height);
                    }
                    break;
#               else
                case V4L2_BUF_TYPE_VIDEO_OUTPUT:
                case V4L2_BUF_TYPE_VIDEO_CAPTURE:
                    if(print){
                        K_LOG_INFO("Buffers(%s), setting pixelformat('%c%c%c%c') width(%u) height(%u) sizeimage(%u) bytesperline(%u).\n", obj->name, pixelformatBytes[0], pixelformatBytes[1], pixelformatBytes[2], pixelformatBytes[3], obj->sp->width, obj->sp->height, obj->sp->sizeimage, obj->sp->bytesperline);
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
                            K_LOG_ERROR("Buffers(%s), setting pixelformat('%c%c%c%c') width(%u) height(%u) returnd(%d).\n", obj->name, pixelformatBytes[0], pixelformatBytes[1], pixelformatBytes[2], pixelformatBytes[3], obj->mp->width, obj->mp->height, rr);
                        }
                        break;
#                   else
                    case V4L2_BUF_TYPE_VIDEO_OUTPUT:
                    case V4L2_BUF_TYPE_VIDEO_CAPTURE:
                        if(print){
                            K_LOG_ERROR("Buffers(%s), setting pixelformat('%c%c%c%c') width(%u) height(%u) sizeimage(%u) bytesperline(%u) returned(%d).\n", obj->name, pixelformatBytes[0], pixelformatBytes[1], pixelformatBytes[2], pixelformatBytes[3], obj->sp->width, obj->sp->height, obj->sp->sizeimage, obj->sp->bytesperline, rr);
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
                            K_LOG_INFO("Buffers(%s), obtained pixelformat('%c%c%c%c') width(%u) height(%u).\n", obj->name, pixelformatBytes[0], pixelformatBytes[1], pixelformatBytes[2], pixelformatBytes[3], obj->mp->width, obj->mp->height);
                        }
                        obj->pixelformat = obj->mp->pixelformat;
                        obj->width  = obj->mp->width;
                        obj->height = obj->mp->height;
                        {
                            int i; for( i = 0; i < obj->mp->num_planes; i++){
                                struct v4l2_plane_pix_format* pp = &obj->mp->plane_fmt[i];
                                if(print){
                                    K_LOG_INFO("    plane #%d, sizeimage(%u) bytesperline(%u).\n", (i + 1), pp->sizeimage, pp->bytesperline);
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
                            K_LOG_INFO("Buffers(%s), obtained pixelformat('%c%c%c%c') width(%u) height(%u) sizeimage(%u) bytesperline(%u).\n", obj->name, pixelformatBytes[0], pixelformatBytes[1], pixelformatBytes[2], pixelformatBytes[3], obj->sp->width, obj->sp->height, obj->sp->sizeimage, obj->sp->bytesperline);
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
        K_LOG_ERROR("Buffers(%s), get-crop errno(%d).\n", obj->name, errno);
        //Note: linux doc, says that some messy drivers only accept one type or the other.
        //      retry with equivalent type.
    } else {
        K_LOG_VERBOSE("Buffers(%s), get-crop: x(%d, +%d) y(%d, +%d).\n", obj->name, sel.r.left, sel.r.width, sel.r.top, sel.r.height);
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
    int areOrphanable = 0;
    struct v4l2_requestbuffers buf;
    memset(&buf, 0, sizeof(buf));
    //
    buf.count = ammount;
    buf.type = obj->type;
    buf.memory = V4L2_MEMORY_MMAP;
    //
    int rr; CALL_IOCTL(rr, v4l2_ioctl(fd, VIDIOC_REQBUFS, &buf));
    if(rr != 0){
        K_LOG_ERROR("Buffers(%s), allocation of %d errno(%d).\n", obj->name, ammount, errno);
    } else {
        if(ammount != buf.count){
            K_LOG_INFO("Buffers(%s), %d of %d allocated.\n", obj->name, buf.count, ammount);
        } else if(ammount != 0){
            K_LOG_VERBOSE("Buffers(%s), %d allocated.\n", obj->name, ammount);
        }
        //orphanable?
#       ifdef V4L2_BUF_CAP_SUPPORTS_ORPHANED_BUFS
        if((buf.capabilities & V4L2_BUF_CAP_SUPPORTS_ORPHANED_BUFS)) areOrphanable = 1;
#       endif
        //capabilities
#       if LINUX_VERSION_CODE >= KERNEL_VERSION(5,0,0)
        if(print){
#           ifdef V4L2_BUF_CAP_SUPPORTS_MMAP
            if((buf.capabilities & V4L2_BUF_CAP_SUPPORTS_MMAP)) K_LOG_INFO("    capability: V4L2_BUF_CAP_SUPPORTS_MMAP.\n");
#           endif
#           ifdef V4L2_BUF_CAP_SUPPORTS_USERPTR
            if((buf.capabilities & V4L2_BUF_CAP_SUPPORTS_USERPTR)) K_LOG_INFO("    capability: V4L2_BUF_CAP_SUPPORTS_USERPTR.\n");
#           endif
#           ifdef V4L2_BUF_CAP_SUPPORTS_DMABUF
            if((buf.capabilities & V4L2_BUF_CAP_SUPPORTS_DMABUF)) K_LOG_INFO("    capability: V4L2_BUF_CAP_SUPPORTS_DMABUF.\n");
#           endif
#           ifdef V4L2_BUF_CAP_SUPPORTS_REQUESTS
            if((buf.capabilities & V4L2_BUF_CAP_SUPPORTS_REQUESTS)) K_LOG_INFO("    capability: V4L2_BUF_CAP_SUPPORTS_REQUESTS.\n");
#           endif
#           ifdef V4L2_BUF_CAP_SUPPORTS_ORPHANED_BUFS
            if((buf.capabilities & V4L2_BUF_CAP_SUPPORTS_ORPHANED_BUFS)) K_LOG_INFO("    capability: V4L2_BUF_CAP_SUPPORTS_ORPHANED_BUFS.\n");
#           endif
#           ifdef V4L2_BUF_CAP_SUPPORTS_M2M_HOLD_CAPTURE_BUF
            if((buf.capabilities & V4L2_BUF_CAP_SUPPORTS_M2M_HOLD_CAPTURE_BUF)) K_LOG_INFO("    capability: V4L2_BUF_CAP_SUPPORTS_M2M_HOLD_CAPTURE_BUF.\n");
#           endif
#           ifdef V4L2_BUF_CAP_SUPPORTS_MMAP_CACHE_HINTS
            if((buf.capabilities & V4L2_BUF_CAP_SUPPORTS_MMAP_CACHE_HINTS)) K_LOG_INFO("    capability: V4L2_BUF_CAP_SUPPORTS_MMAP_CACHE_HINTS.\n");
#           endif
            if(buf.capabilities != 0){
                K_LOG_INFO("    capabilities: %d.\n", buf.capabilities);
            }
        }
#       endif
        //flags
#       if LINUX_VERSION_CODE >= KERNEL_VERSION(6,0,0)
        if(print){
#           ifdef V4L2_MEMORY_FLAG_NON_COHERENT
            if((buf.flags & V4L2_MEMORY_FLAG_NON_COHERENT)) K_LOG_INFO("    flag: V4L2_MEMORY_FLAG_NON_COHERENT.\n");
#           endif
            if(buf.flags != 0){
                K_LOG_INFO("    flags: %d.\n", buf.flags);
            }
        }
#       endif
        //map
        r = 0;
        {
            //release previous buffers
            {
                if(obj->arr != NULL){
                    int i; for(i = 0; i < obj->sz; i++){
                        Buffer_release(&obj->arr[i]);
                    }
                    free(obj->arr);
                    obj->arr = NULL;
                }
                obj->sz             = 0;
                obj->enqueuedCount  = 0;
                obj->lastDequeued   = NULL;
                obj->isLastDequeuedCloned = 0;
            }
            //allocate new buffers
            {
                int planesAmm = 0;
                switch(obj->type){
    #               ifdef K_USE_MPLANE
                    case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
                    case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
                        planesAmm = obj->mp->num_planes;
                        break;
    #               else
                    case V4L2_BUF_TYPE_VIDEO_OUTPUT:
                    case V4L2_BUF_TYPE_VIDEO_CAPTURE:
                        planesAmm = 1;
                        break;
    #               endif
                    default:
                        break;
                }
                if(buf.count > 0){
                    obj->arr = (STBuffer*)malloc(sizeof(STBuffer) * buf.count);
                    if(obj->arr == NULL){
                        K_LOG_INFO("Buffers(%s), allocBuffs, malloc-bufers fail.\n", obj->name);
                        r = -1;
                    } else {
                        int i; for(i = 0; i < buf.count; i++){
                            STBuffer* b = &obj->arr[i];
                            Buffer_init(b);
                            b->index = i;
                            b->isOrphanable = areOrphanable;
                            IF_DEBUG(b->dbg.indexPlusOne = (b->index + 1);)
                            obj->sz++;
                            //allocate planes
                            if(planesAmm > 0){
                                b->planes = malloc(sizeof(STPlane) * planesAmm);
                                if(b->planes == NULL){
                                    K_LOG_INFO("Buffers(%s), allocBuffs, malloc-planes fail.\n", obj->name);
                                    r = -1;
                                    break;
                                } else {
                                    int j; for(j = 0; j < planesAmm; j++){
                                        STPlane* p = &b->planes[j];
                                        Plane_init(p);
                                        p->isOrphanable = b->isOrphanable;
                                        b->planesSz++;
                                    }
                                }
                            }
                        }
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
    int i; for(i = 0; i < obj->sz; i++){
        STBuffer* buffer = &obj->arr[i];
        struct v4l2_buffer* srchBuff = &obj->srchBuff;
        K_ASSERT(buffer->dbg.indexPlusOne == (buffer->index + 1))
        //prepare search record
        memset(srchBuff, 0, sizeof(*srchBuff));
        srchBuff->index = i;
        srchBuff->type  = obj->type;
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
            K_LOG_ERROR("Buffers(%s), VIDIOC_QUERYBUF errno(%d).\n", obj->name, errno);
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
            K_ASSERT(buffer->planesSz == planesSz) //must be same size
            if(buffer->planesSz != planesSz){
                K_LOG_ERROR("Buffers(%s) (#%d/%d) expected %d planes (found %d).\n", obj->name, (i + 1), obj->sz, planesSz, buffer->planesSz);
            } else {
                //apply
                int j; for(j = 0; j < buffer->planesSz; j++){
                    STPlane* plane  = &buffer->planes[j];
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
                                K_LOG_INFO("Buffers(%s) (#%d/%d) export is not supported.\n", obj->name, (i + 1), obj->sz);
                            } else {
                                const STErrCode* err = _getErrCode(errno);
                                if(err == NULL){
                                    K_LOG_ERROR("Buffers(%s) (#%d/%d) plane(#%d/%d) export for DMA returned errno(%d).\n", obj->name, (i + 1), obj->sz, j + 1, buffer->planesSz, errno);
                                } else {
                                    K_LOG_ERROR("Buffers(%s) (#%d/%d) plane(#%d/%d) export for DMA returned '%s'.\n", obj->name, (i + 1), obj->sz, j + 1, buffer->planesSz, err->str);
                                }
                                r = -1;
                            }
                        } else {
                            K_LOG_INFO("Buffers(%s) (#%d/%d) plane(#%d/%d) exported for DMA file(%d) dma(%d).\n", obj->name, (i + 1), obj->sz, j + 1, buffer->planesSz, fd, expbuf.fd);
                            if(plane->fd >= 0){
                                if(plane->isOrphanable){
                                    close(plane->fd);
                                }
                                plane->fd = -1;
                            }
                            plane->fd = expbuf.fd;
                        }
                    }
                }
            }
        }
    }
    return r;
}

int Buffers_mmap(STBuffers* obj, int fd){
    int r = 0;
    int i; for(i = 0; i < obj->sz; i++){
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
            K_LOG_ERROR("Buffers(%s), VIDIOC_QUERYBUF errno(%d).\n", obj->name, errno);
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
            K_ASSERT(buffer->planesSz == planesSz) //must be same size
            if(buffer->planesSz != planesSz){
                K_LOG_ERROR("Buffers(%s) (#%d/%d) expected %d planes (found %d).\n", obj->name, (i + 1), obj->sz, planesSz, buffer->planesSz);
            } else {
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
                            K_LOG_INFO("Buffers(%s) (#%d/%d) plane(#%d/%d) is not page aligned lenght(%d) correctedLen(%d).\n", obj->name, (i + 1), obj->sz, j + 1, buffer->planesSz, (unsigned int)pLength, (unsigned int)pa_len);
                            r = -1;
                        }
#                       ifdef K_IS_JETSON_NANO
                        int fddd = (plane->fd >= 0 ? plane->fd : fd); //whyyyyyyyy?
#                       else
                        int fddd = (/*plane->fd >= 0 ? plane->fd :*/ fd); //whyyyyyyyy?
#                       endif
                        void* rrmap = v4l2_mmap(NULL, pLength, PROT_READ | PROT_WRITE, MAP_SHARED, fddd, pMemOffset);
                        if(rrmap == MAP_FAILED){
                            if(errno == EACCES) { K_LOG_ERROR("buffers(%s) (#%d/%d) plane(#%d/%d) map failed to myFd(%d) enumFd(%d) length(%d) mem_offset(%d): EACCES.\n", obj->name, (i + 1), obj->sz, j + 1, buffer->planesSz, plane->fd, enumFd, pLength, pMemOffset); }
                            if(errno == EAGAIN) { K_LOG_ERROR("buffers(%s) (#%d/%d) plane(#%d/%d) map failed to myFd(%d) enumFd(%d) length(%d) mem_offset(%d): EAGAIN.\n", obj->name, (i + 1), obj->sz, j + 1, buffer->planesSz, plane->fd, enumFd, pLength, pMemOffset); }
                            if(errno == EBADF) { K_LOG_ERROR("buffers(%s) (#%d/%d) plane(#%d/%d) map failed to myFd(%d) enumFd(%d) length(%d) mem_offset(%d): EBADF.\n", obj->name, (i + 1), obj->sz, j + 1, buffer->planesSz, plane->fd, enumFd, pLength, pMemOffset); }
                            if(errno == EEXIST) { K_LOG_ERROR("buffers(%s) (#%d/%d) plane(#%d/%d) map failed to myFd(%d) enumFd(%d) length(%d) mem_offset(%d): EEXIST.\n", obj->name, (i + 1), obj->sz, j + 1, buffer->planesSz, plane->fd, enumFd, pLength, pMemOffset); }
                            if(errno == EINVAL) { K_LOG_ERROR("buffers(%s) (#%d/%d) plane(#%d/%d) map failed to myFd(%d) enumFd(%d) length(%d) mem_offset(%d): EINVAL.\n", obj->name, (i + 1), obj->sz, j + 1, buffer->planesSz, plane->fd, enumFd, pLength, pMemOffset); }
                            if(errno == ENFILE) { K_LOG_ERROR("buffers(%s) (#%d/%d) plane(#%d/%d) map failed to myFd(%d) enumFd(%d) length(%d) mem_offset(%d): ENFILE.\n", obj->name, (i + 1), obj->sz, j + 1, buffer->planesSz, plane->fd, enumFd, pLength, pMemOffset); }
                            if(errno == ENODEV) { K_LOG_ERROR("buffers(%s) (#%d/%d) plane(#%d/%d) map failed to myFd(%d) enumFd(%d) length(%d) mem_offset(%d): ENODEV.\n", obj->name, (i + 1), obj->sz, j + 1, buffer->planesSz, plane->fd, enumFd, pLength, pMemOffset); }
                            if(errno == ENOMEM) { K_LOG_ERROR("buffers(%s) (#%d/%d) plane(#%d/%d) map failed to myFd(%d) enumFd(%d) length(%d) mem_offset(%d): ENOMEM.\n", obj->name, (i + 1), obj->sz, j + 1, buffer->planesSz, plane->fd, enumFd, pLength, pMemOffset); }
                            if(errno == EOVERFLOW) { K_LOG_ERROR("buffers(%s) (#%d/%d) plane(#%d/%d) map failed to myFd(%d) enumFd(%d) length(%d) mem_offset(%d): EOVERFLOW.\n", obj->name, (i + 1), obj->sz, j + 1, buffer->planesSz, plane->fd, enumFd, pLength, pMemOffset); }
                            if(errno == EPERM) { K_LOG_ERROR("buffers(%s) (#%d/%d) plane(#%d/%d) map failed to myFd(%d) enumFd(%d) length(%d) mem_offset(%d): EPERM.\n", obj->name, (i + 1), obj->sz, j + 1, buffer->planesSz, plane->fd, enumFd, pLength, pMemOffset); }
                            if(errno == ETXTBSY) { K_LOG_ERROR("buffers(%s) (#%d/%d) plane(#%d/%d) map failed to myFd(%d) enumFd(%d) length(%d) mem_offset(%d): ETXTBSY.\n", obj->name, (i + 1), obj->sz, j + 1, buffer->planesSz, plane->fd, enumFd, pLength, pMemOffset); }
#                           ifdef SIGSEGV
                            if(errno == SIGSEGV) { K_LOG_ERROR("buffers(%s) (#%d/%d) plane(#%d/%d) map failed to myFd(%d) enumFd(%d) length(%d) mem_offset(%d): SIGSEGV.\n", obj->name, (i + 1), obj->sz, j + 1, buffer->planesSz, plane->fd, enumFd, pLength, pMemOffset); }
#                           endif
#                           ifdef SIGBUS
                            if(errno == SIGBUS) { K_LOG_ERROR("buffers(%s) (#%d/%d) plane(#%d/%d) map failed to myFd(%d) enumFd(%d) length(%d) mem_offset(%d): SIGBUS.\n", obj->name, (i + 1), obj->sz, j + 1, buffer->planesSz, plane->fd, enumFd, pLength, pMemOffset); }
#                           endif
                            r = -1;
                            break;
                        } else {
                            K_LOG_VERBOSE("Buffers(%s) (#%d/%d) plane(#%d/%d) mapped to (%llu) myFd(%d) enumFd(%d) length(%d) mem_offset(%d).\n", obj->name, (i + 1), obj->sz, j + 1, buffer->planesSz, (unsigned long)rrmap, plane->fd, enumFd, pLength, pMemOffset);
                            //release previous
                            if(plane->dataPtr != NULL){
                                if(plane->isMmaped){
                                    if(plane->isOrphanable){
                                        int rr = v4l2_munmap(plane->dataPtr, plane->length);
                                        if(rr == 0){
                                            K_LOG_VERBOSE("Buffers(%s) (#%d/%d) plane(#%d/%d) unmapped addr(%llu) len(%u).\n", obj->name, (i + 1), obj->sz, (j + 1), buffer->planesSz, (unsigned long)plane->dataPtr, plane->length);
                                        } else {
                                            K_LOG_ERROR("bufers(%s) munmap returned(%d) for buffer(#%d/%d) plane(#%d/%d) addr(%llu) len(%u).\n", obj->name, rr, (i + 1), obj->sz, (j + 1), buffer->planesSz, (unsigned long)plane->dataPtr, plane->length);
                                        }
                                    }
                                } else {
                                    free(plane->dataPtr);
                                }
                                plane->isMmaped = 0;
                                plane->dataPtr = NULL;
                                plane->length = 0;
                                plane->memOffset = 0;
                            }
                            plane->isMmaped = 1;
                            plane->dataPtr  = (unsigned char*)rrmap;
                            plane->length   = pLength;
                            plane->memOffset = pMemOffset;
                        }
                    }
                }
            }
        }
    }
    return r;
}

int Buffers_enqueueMinimun(STBuffers* obj, int fd, const int minimun){
    struct v4l2_buffer* srchBuff = &obj->srchBuff;
    while(obj->enqueuedCount < minimun){
        STBuffer* bufferQueued = NULL;
        int i; for(i = 0; i < obj->sz && obj->enqueuedCount < minimun; i++){
            STBuffer* buffer = &obj->arr[i];
            K_ASSERT(buffer->dbg.indexPlusOne == (buffer->index + 1))
            if(!buffer->isQueued){
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
                        K_LOG_ERROR("Buffers(%s), #%d/%d queeing errno(%d).\n", obj->name, (i + 1), obj->sz, errno);
                    } else {
                        K_LOG_VERBOSE("Buffers(%s), #%d/%d queued.\n", obj->name, (i + 1), obj->sz);
                        buffer->isQueued = 1;
                        obj->enqueuedCount++; K_ASSERT(obj->enqueuedCount >= 0 && obj->enqueuedCount <= obj->sz);
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
    int i; for(i = 0; i < obj->sz; i++){
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
                K_LOG_ERROR("Buffers(%s), queueing buffer(#%d/%d) errno(%d).\n", obj->name, srchBuff->index + 1, obj->sz, errno);
            } else {
                K_LOG_VERBOSE("Buffers(%s), queueing new-buffer(#%d/%d) success.\n", obj->name, srchBuff->index + 1, obj->sz);
                buffer->isQueued = 1;
                obj->enqueuedCount++; K_ASSERT(obj->enqueuedCount >= 0 && obj->enqueuedCount <= obj->sz)
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
    int rr = v4l2_ioctl(fd, VIDIOC_DQBUF, &obj->srchBuff);
    if(rr != 0){
        switch (errno) {
            case EAGAIN:
                //Non-blocking IO
                K_LOG_VERBOSE("Buffers(%s), Unqueueing buffer (returned EAGAIN, no buffer is ready).\n", obj->name);
                break;
            case EINVAL:
                //The buffer type is not supported, or the index is out of bounds, or no buffers have been allocated yet, or the userptr or length are invalid.
                K_LOG_ERROR("Buffers(%s), Unqueueing buffer (returned EINVAL, no buffer is ready).\n", obj->name);
                break;
            case EIO:
                //VIDIOC_DQBUF failed due to an internal error. Can also indicate temporary problems like signal loss.
                K_LOG_ERROR("Buffers(%s), Unqueueing buffer (returned EIO, no buffer is ready).\n", obj->name);
                break;
            case EPIPE:
                //VIDIOC_DQBUF returns this on an empty capture queue for mem2mem codecs if a buffer with the V4L2_BUF_FLAG_LAST was already dequeued and no new buffers are expected to become available.
                K_LOG_ERROR("Buffers(%s), Unqueueing buffer (returned EPIPE, last buffer given, dst-restart is required).\n", obj->name);
                /*if(wasEPIPE != NULL){
                    *wasEPIPE = 1;
                }*/
                break;
            default:
                {
                    const STErrCode* err = _getErrCode(errno);
                    if(err == NULL){
                        K_LOG_ERROR("Buffers(%s), Unqueueing buffer returned errno(%d).\n", obj->name, errno);
                    } else {
                        K_LOG_ERROR("Buffers(%s), Unqueueing buffer returned '%s'.\n", obj->name, err->str);
                    }
                }
                break;
        }
    } else if(srchBuff->index >= obj->sz){
        K_LOG_ERROR("Buffers(%s), dequeued returned an invalid buffer-index.\n", obj->name);
    } else {
        K_LOG_VERBOSE("Unqueueing dst-buffer(#%d/%d) returned filled.\n", obj->name, (srchBuff->index + 1), obj->sz);
        STBuffer* buffer = &obj->arr[srchBuff->index];
        //
        K_ASSERT(buffer->dbg.indexPlusOne == (buffer->index + 1))
        K_ASSERT(buffer->isQueued)
        //sync state
        obj->enqueuedCount--; K_ASSERT(obj->enqueuedCount >= 0 && obj->enqueuedCount <= obj->sz)
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
         K_LOG_INFO("Buffers(%s), V4L2_BUF_FLAG_LAST found but unexpected (no active drainig-seq).\n", obj->name);
         } else {
         K_LOG_INFO("Buffers(%s), drain completed (V4L2_BUF_FLAG_LAST).\n", obj->name);
         }
         if(dstWasDrainLastBuff != NULL){
         *dstWasDrainLastBuff = 1;
         }
         }*/
        //flags
        /*{
         if((srchBuff->flags & V4L2_BUF_FLAG_MAPPED)){ K_LOG_INFO("    flag: V4L2_BUF_FLAG_MAPPED.\n"); }
         if((srchBuff->flags & V4L2_BUF_FLAG_QUEUED)){ K_LOG_INFO("    flag: V4L2_BUF_FLAG_QUEUED.\n"); }
         if((srchBuff->flags & V4L2_BUF_FLAG_DONE)){ K_LOG_INFO("    flag: V4L2_BUF_FLAG_DONE.\n"); }
         if((srchBuff->flags & V4L2_BUF_FLAG_ERROR)){ K_LOG_INFO("    flag: V4L2_BUF_FLAG_ERROR.\n"); }
         #           ifdef V4L2_BUF_FLAG_IN_REQUEST
         if((srchBuff->flags & V4L2_BUF_FLAG_IN_REQUEST)){ K_LOG_INFO("    flag: V4L2_BUF_FLAG_IN_REQUEST.\n"); }
         #           endif
         if((srchBuff->flags & V4L2_BUF_FLAG_KEYFRAME)){ K_LOG_INFO("    flag: V4L2_BUF_FLAG_KEYFRAME.\n"); }
         if((srchBuff->flags & V4L2_BUF_FLAG_PFRAME)){ K_LOG_INFO("    flag: V4L2_BUF_FLAG_PFRAME.\n"); }
         if((srchBuff->flags & V4L2_BUF_FLAG_BFRAME)){ K_LOG_INFO("    flag: V4L2_BUF_FLAG_BFRAME.\n"); }
         if((srchBuff->flags & V4L2_BUF_FLAG_TIMECODE)){ K_LOG_INFO("    flag: V4L2_BUF_FLAG_TIMECODE.\n"); }
         if((srchBuff->flags & V4L2_BUF_FLAG_PREPARED)){ K_LOG_INFO("    flag: V4L2_BUF_FLAG_PREPARED.\n"); }
         if((srchBuff->flags & V4L2_BUF_FLAG_NO_CACHE_INVALIDATE)){ K_LOG_INFO("    flag: V4L2_BUF_FLAG_NO_CACHE_INVALIDATE.\n"); }
         if((srchBuff->flags & V4L2_BUF_FLAG_NO_CACHE_CLEAN)){ K_LOG_INFO("    flag: V4L2_BUF_FLAG_NO_CACHE_CLEAN.\n"); }
         #           ifdef V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF
         if((srchBuff->flags & V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF)){ K_LOG_INFO("    flag: V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF.\n"); }
         #           endif
         if((srchBuff->flags & V4L2_BUF_FLAG_LAST)){ K_LOG_INFO("    flag: V4L2_BUF_FLAG_LAST.\n"); }
         #           ifdef V4L2_BUF_FLAG_REQUEST_FD
         if((srchBuff->flags & V4L2_BUF_FLAG_REQUEST_FD)){ K_LOG_INFO("    flag: V4L2_BUF_FLAG_REQUEST_FD.\n"); }
         #           endif
         if((srchBuff->flags & V4L2_BUF_FLAG_TIMESTAMP_MASK)){ K_LOG_INFO("    flag: V4L2_BUF_FLAG_TIMESTAMP_MASK.\n"); }
         if((srchBuff->flags & V4L2_BUF_FLAG_TIMESTAMP_UNKNOWN)){ K_LOG_INFO("    flag: V4L2_BUF_FLAG_TIMESTAMP_UNKNOWN.\n"); }
         if((srchBuff->flags & V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC)){ K_LOG_INFO("    flag: V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC.\n"); }
         if((srchBuff->flags & V4L2_BUF_FLAG_TIMESTAMP_COPY)){ K_LOG_INFO("    flag: V4L2_BUF_FLAG_TIMESTAMP_COPY.\n"); }
         if((srchBuff->flags & V4L2_BUF_FLAG_TSTAMP_SRC_MASK)){ K_LOG_INFO("    flag: V4L2_BUF_FLAG_TSTAMP_SRC_MASK.\n"); }
         if((srchBuff->flags & V4L2_BUF_FLAG_TSTAMP_SRC_EOF)){ K_LOG_INFO("    flag: V4L2_BUF_FLAG_TSTAMP_SRC_EOF.\n"); }
         if((srchBuff->flags & V4L2_BUF_FLAG_TSTAMP_SRC_SOE)){ K_LOG_INFO("    flag: V4L2_BUF_FLAG_TSTAMP_SRC_SOE.\n"); }
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
        K_LOG_ERROR("Buffers(%s) start errno(%d).\n", obj->name, errno);
    } else {
        obj->isExplicitON = 1;
        obj->isImplicitON = 1;
        K_LOG_VERBOSE("Buffers_start success.\n");
    }
    return r;
}

int Buffers_stop(STBuffers* obj, int fd){
    int type = obj->type;
    int r; CALL_IOCTL(r, v4l2_ioctl(fd, VIDIOC_STREAMOFF, &type));
    if(r != 0){
        K_LOG_ERROR("Buffers(%s) stop errno(%d).\n", obj->name, errno);
    } else {
        obj->isExplicitON = 0;
        obj->isImplicitON = 0;
        K_LOG_VERBOSE("Buffers(%s) stop success.\n", obj->name);
        //flag all buffers as dequeued
        {
            int i; for(i = 0; i < obj->sz; i++){
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
        K_LOG_ERROR("v4lDevice VIDIOC_QUERYCAP erron(%d).\n", errno);
    } else {
        if(print){
            K_LOG_INFO("----------------.\n");
            K_LOG_INFO("---- DEVICE ----.\n");
            K_LOG_INFO("----------------.\n");
            K_LOG_INFO("Driver: '%s'.\n", cap.driver);
            K_LOG_INFO("  Card: '%s'.\n", cap.card);
            K_LOG_INFO("   Bus: '%s'.\n", cap.bus_info);
            K_LOG_INFO("   Ver: %u.%u.%u.\n", (cap.version >> 16) & 0xFF, (cap.version >> 8) & 0xFF, cap.version & 0xFF);
            K_LOG_INFO("   Cap: .\n");
#       ifdef V4L2_CAP_VIDEO_CAPTURE
            if((cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)){ K_LOG_INFO("   Cap: V4L2_CAP_VIDEO_CAPTURE.\n"); }
#       endif
#       ifdef V4L2_CAP_VIDEO_CAPTURE_MPLANE
            if((cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE)){ K_LOG_INFO("   Cap: V4L2_CAP_VIDEO_CAPTURE_MPLANE.\n"); }
#       endif
#       ifdef V4L2_CAP_VIDEO_OUTPUT
            if((cap.capabilities & V4L2_CAP_VIDEO_OUTPUT)){ K_LOG_INFO("   Cap: V4L2_CAP_VIDEO_OUTPUT.\n"); }
#       endif
#       ifdef V4L2_CAP_VIDEO_OUTPUT_MPLANE
            if((cap.capabilities & V4L2_CAP_VIDEO_OUTPUT_MPLANE)){ K_LOG_INFO("   Cap: V4L2_CAP_VIDEO_OUTPUT_MPLANE.\n"); }
#       endif
#       ifdef V4L2_CAP_VIDEO_M2M
            if((cap.capabilities & V4L2_CAP_VIDEO_M2M)){ K_LOG_INFO("   Cap: V4L2_CAP_VIDEO_M2M.\n"); }
#       endif
#       ifdef V4L2_CAP_VIDEO_M2M_MPLANE
            if((cap.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE)){ K_LOG_INFO("   Cap: V4L2_CAP_VIDEO_M2M_MPLANE.\n"); }
#       endif
#       ifdef V4L2_CAP_VIDEO_OVERLAY
            if((cap.capabilities & V4L2_CAP_VIDEO_OVERLAY)){ K_LOG_INFO("   Cap: V4L2_CAP_VIDEO_OVERLAY.\n"); }
#       endif
#       ifdef V4L2_CAP_VBI_CAPTURE
            if((cap.capabilities & V4L2_CAP_VBI_CAPTURE)){ K_LOG_INFO("   Cap: V4L2_CAP_VBI_CAPTURE.\n"); }
#       endif
#       ifdef V4L2_CAP_VBI_OUTPUT
            if((cap.capabilities & V4L2_CAP_VBI_OUTPUT)){ K_LOG_INFO("   Cap: V4L2_CAP_VBI_OUTPUT.\n"); }
#       endif
#       ifdef V4L2_CAP_SLICED_VBI_CAPTURE
            if((cap.capabilities & V4L2_CAP_SLICED_VBI_CAPTURE)){ K_LOG_INFO("   Cap: V4L2_CAP_SLICED_VBI_CAPTURE.\n"); }
#       endif
#       ifdef V4L2_CAP_SLICED_VBI_OUTPUT
            if((cap.capabilities & V4L2_CAP_SLICED_VBI_OUTPUT)){ K_LOG_INFO("   Cap: V4L2_CAP_SLICED_VBI_OUTPUT.\n"); }
#       endif
#       ifdef V4L2_CAP_RDS_CAPTURE
            if((cap.capabilities & V4L2_CAP_RDS_CAPTURE)){ K_LOG_INFO("   Cap: V4L2_CAP_RDS_CAPTURE.\n"); }
#       endif
#       ifdef V4L2_CAP_VIDEO_OUTPUT_OVERLAY
            if((cap.capabilities & V4L2_CAP_VIDEO_OUTPUT_OVERLAY)){ K_LOG_INFO("   Cap: V4L2_CAP_VIDEO_OUTPUT_OVERLAY.\n"); }
#       endif
#       ifdef V4L2_CAP_HW_FREQ_SEEK
            if((cap.capabilities & V4L2_CAP_HW_FREQ_SEEK)){ K_LOG_INFO("   Cap: V4L2_CAP_HW_FREQ_SEEK.\n"); }
#       endif
#       ifdef V4L2_CAP_RDS_OUTPUT
            if((cap.capabilities & V4L2_CAP_RDS_OUTPUT)){ K_LOG_INFO("   Cap: V4L2_CAP_RDS_OUTPUT.\n"); }
#       endif
#       ifdef V4L2_CAP_TUNER
            if((cap.capabilities & V4L2_CAP_TUNER)){ K_LOG_INFO("   Cap: V4L2_CAP_TUNER.\n"); }
#       endif
#       ifdef V4L2_CAP_AUDIO
            if((cap.capabilities & V4L2_CAP_AUDIO)){ K_LOG_INFO("   Cap: V4L2_CAP_AUDIO.\n"); }
#       endif
#       ifdef V4L2_CAP_RADIO
            if((cap.capabilities & V4L2_CAP_RADIO)){ K_LOG_INFO("   Cap: V4L2_CAP_RADIO.\n"); }
#       endif
#       ifdef V4L2_CAP_MODULATOR
            if((cap.capabilities & V4L2_CAP_MODULATOR)){ K_LOG_INFO("   Cap: V4L2_CAP_MODULATOR.\n"); }
#       endif
#       ifdef V4L2_CAP_SDR_CAPTURE
            if((cap.capabilities & V4L2_CAP_SDR_CAPTURE)){ K_LOG_INFO("   Cap: V4L2_CAP_SDR_CAPTURE.\n"); }
#       endif
#       ifdef V4L2_CAP_EXT_PIX_FORMAT
            if((cap.capabilities & V4L2_CAP_EXT_PIX_FORMAT)){ K_LOG_INFO("   Cap: V4L2_CAP_EXT_PIX_FORMAT.\n"); }
#       endif
#       ifdef V4L2_CAP_SDR_OUTPUT
            if((cap.capabilities & V4L2_CAP_SDR_OUTPUT)){ K_LOG_INFO("   Cap: V4L2_CAP_SDR_OUTPUT.\n"); }
#       endif
#       ifdef V4L2_CAP_READWRITE
            if((cap.capabilities & V4L2_CAP_READWRITE)){ K_LOG_INFO("   Cap: V4L2_CAP_READWRITE.\n"); }
#       endif
#       ifdef V4L2_CAP_ASYNCIO
            if((cap.capabilities & V4L2_CAP_ASYNCIO)){ K_LOG_INFO("   Cap: V4L2_CAP_ASYNCIO.\n"); }
#       endif
#       ifdef V4L2_CAP_STREAMING
            if((cap.capabilities & V4L2_CAP_STREAMING)){ K_LOG_INFO("   Cap: V4L2_CAP_STREAMING.\n"); }
#       endif
#       ifdef V4L2_CAP_TOUCH
            if((cap.capabilities & V4L2_CAP_TOUCH)){ K_LOG_INFO("   Cap: V4L2_CAP_TOUCH.\n"); }
#       endif
#       ifdef V4L2_CAP_DEVICE_CAPS
            if((cap.capabilities & V4L2_CAP_DEVICE_CAPS)){
                K_LOG_INFO("   Cap: V4L2_CAP_DEVICE_CAPS.\n");
                {
#              ifdef V4L2_CAP_VIDEO_CAPTURE
                    if((cap.device_caps & V4L2_CAP_VIDEO_CAPTURE)){ K_LOG_INFO("DevCap: V4L2_CAP_VIDEO_CAPTURE.\n"); }
#              endif
#              ifdef V4L2_CAP_VIDEO_CAPTURE_MPLANE
                    if((cap.device_caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE)){ K_LOG_INFO("DevCap: V4L2_CAP_VIDEO_CAPTURE_MPLANE.\n"); }
#              endif
#              ifdef V4L2_CAP_VIDEO_OUTPUT
                    if((cap.device_caps & V4L2_CAP_VIDEO_OUTPUT)){ K_LOG_INFO("DevCap: V4L2_CAP_VIDEO_OUTPUT.\n"); }
#              endif
#              ifdef V4L2_CAP_VIDEO_OUTPUT_MPLANE
                    if((cap.device_caps & V4L2_CAP_VIDEO_OUTPUT_MPLANE)){ K_LOG_INFO("DevCap: V4L2_CAP_VIDEO_OUTPUT_MPLANE.\n"); }
#              endif
#              ifdef V4L2_CAP_VIDEO_M2M
                    if((cap.device_caps & V4L2_CAP_VIDEO_M2M)){ K_LOG_INFO("DevCap: V4L2_CAP_VIDEO_M2M.\n"); }
#              endif
#              ifdef V4L2_CAP_VIDEO_M2M_MPLANE
                    if((cap.device_caps & V4L2_CAP_VIDEO_M2M_MPLANE)){ K_LOG_INFO("DevCap: V4L2_CAP_VIDEO_M2M_MPLANE.\n"); }
#              endif
#              ifdef V4L2_CAP_VIDEO_OVERLAY
                    if((cap.device_caps & V4L2_CAP_VIDEO_OVERLAY)){ K_LOG_INFO("DevCap: V4L2_CAP_VIDEO_OVERLAY.\n"); }
#              endif
#              ifdef V4L2_CAP_VBI_CAPTURE
                    if((cap.device_caps & V4L2_CAP_VBI_CAPTURE)){ K_LOG_INFO("DevCap: V4L2_CAP_VBI_CAPTURE.\n"); }
#              endif
#              ifdef V4L2_CAP_VBI_OUTPUT
                    if((cap.device_caps & V4L2_CAP_VBI_OUTPUT)){ K_LOG_INFO("DevCap: V4L2_CAP_VBI_OUTPUT.\n"); }
#              endif
#              ifdef V4L2_CAP_SLICED_VBI_CAPTURE
                    if((cap.device_caps & V4L2_CAP_SLICED_VBI_CAPTURE)){ K_LOG_INFO("DevCap: V4L2_CAP_SLICED_VBI_CAPTURE.\n"); }
#              endif
#              ifdef V4L2_CAP_SLICED_VBI_OUTPUT
                    if((cap.device_caps & V4L2_CAP_SLICED_VBI_OUTPUT)){ K_LOG_INFO("DevCap: V4L2_CAP_SLICED_VBI_OUTPUT.\n"); }
#              endif
#              ifdef V4L2_CAP_RDS_CAPTURE
                    if((cap.device_caps & V4L2_CAP_RDS_CAPTURE)){ K_LOG_INFO("DevCap: V4L2_CAP_RDS_CAPTURE.\n"); }
#              endif
#              ifdef V4L2_CAP_VIDEO_OUTPUT_OVERLAY
                    if((cap.device_caps & V4L2_CAP_VIDEO_OUTPUT_OVERLAY)){ K_LOG_INFO("DevCap: V4L2_CAP_VIDEO_OUTPUT_OVERLAY.\n"); }
#              endif
#              ifdef V4L2_CAP_HW_FREQ_SEEK
                    if((cap.device_caps & V4L2_CAP_HW_FREQ_SEEK)){ K_LOG_INFO("DevCap: V4L2_CAP_HW_FREQ_SEEK.\n"); }
#              endif
#              ifdef V4L2_CAP_RDS_OUTPUT
                    if((cap.device_caps & V4L2_CAP_RDS_OUTPUT)){ K_LOG_INFO("DevCap: V4L2_CAP_RDS_OUTPUT.\n"); }
#              endif
#              ifdef V4L2_CAP_TUNER
                    if((cap.device_caps & V4L2_CAP_TUNER)){ K_LOG_INFO("DevCap: V4L2_CAP_TUNER.\n"); }
#              endif
#              ifdef V4L2_CAP_AUDIO
                    if((cap.device_caps & V4L2_CAP_AUDIO)){ K_LOG_INFO("DevCap: V4L2_CAP_AUDIO.\n"); }
#              endif
#              ifdef V4L2_CAP_RADIO
                    if((cap.device_caps & V4L2_CAP_RADIO)){ K_LOG_INFO("DevCap: V4L2_CAP_RADIO.\n"); }
#              endif
#              ifdef V4L2_CAP_MODULATOR
                    if((cap.device_caps & V4L2_CAP_MODULATOR)){ K_LOG_INFO("DevCap: V4L2_CAP_MODULATOR.\n"); }
#              endif
#              ifdef V4L2_CAP_SDR_CAPTURE
                    if((cap.device_caps & V4L2_CAP_SDR_CAPTURE)){ K_LOG_INFO("DevCap: V4L2_CAP_SDR_CAPTURE.\n"); }
#              endif
#              ifdef V4L2_CAP_EXT_PIX_FORMAT
                    if((cap.device_caps & V4L2_CAP_EXT_PIX_FORMAT)){ K_LOG_INFO("DevCap: V4L2_CAP_EXT_PIX_FORMAT.\n"); }
#              endif
#              ifdef V4L2_CAP_SDR_OUTPUT
                    if((cap.device_caps & V4L2_CAP_SDR_OUTPUT)){ K_LOG_INFO("DevCap: V4L2_CAP_SDR_OUTPUT.\n"); }
#              endif
#              ifdef V4L2_CAP_READWRITE
                    if((cap.device_caps & V4L2_CAP_READWRITE)){ K_LOG_INFO("DevCap: V4L2_CAP_READWRITE.\n"); }
#              endif
#              ifdef V4L2_CAP_ASYNCIO
                    if((cap.device_caps & V4L2_CAP_ASYNCIO)){ K_LOG_INFO("DevCap: V4L2_CAP_ASYNCIO.\n"); }
#              endif
#              ifdef V4L2_CAP_STREAMING
                    if((cap.device_caps & V4L2_CAP_STREAMING)){ K_LOG_INFO("DevCap: V4L2_CAP_STREAMING.\n"); }
#              endif
#              ifdef V4L2_CAP_TOUCH
                    if((cap.device_caps & V4L2_CAP_TOUCH)){ K_LOG_INFO("DevCap: V4L2_CAP_TOUCH.\n"); }
#              endif
#              ifdef V4L2_CAP_DEVICE_CAPS
                    if((cap.device_caps & V4L2_CAP_DEVICE_CAPS)){ K_LOG_INFO("DevCap: V4L2_CAP_DEVICE_CAPS.\n"); }
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
            K_LOG_INFO("%d controls queried (%d standard, %d private requested).\n", controlsTotal, reqsCountBase, reqsCountPriv);
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
                K_LOG_VERBOSE("Control %d value: %d.\n", cid, ctrl.value);
                controlsTotal++;
            }
        }
        //private controls
        /*{
            rr2 = 0; ctrl.id = V4L2_CID_PRIVATE_BASE;
            while(rr2 == 0){
                rr2 = v4l2_ioctl(fd, VIDIOC_G_CTRL, &ctrl);
                if(rr2 == 0){
                    K_LOG_INFO("Control %d value: %d.\n", ctrl.id, ctrl.value);
                    controlsTotal++;
                }
                //next
                ctrl.id++;
            }
        }*/
        if(print){
            K_LOG_INFO("%d controls goten.\n", controlsTotal);
        }
    }
    return 0;
}

int v4lDevice_controlAnalyze(int fd, struct v4l2_queryctrl* ctrl, const int print){
    if(print){
        K_LOG_INFO("Control: '%s' (%d, +%d, %d).\n", ctrl->name, ctrl->minimum, ctrl->step, ctrl->maximum);
        //type
        switch(ctrl->type){
            case V4L2_CTRL_TYPE_INTEGER: K_LOG_INFO("    Type: V4L2_CTRL_TYPE_INTEGER.\n"); break;
            case V4L2_CTRL_TYPE_BOOLEAN: K_LOG_INFO("    Type: V4L2_CTRL_TYPE_BOOLEAN.\n"); break;
            case V4L2_CTRL_TYPE_MENU: K_LOG_INFO("    Type: V4L2_CTRL_TYPE_MENU.\n"); break;
            case V4L2_CTRL_TYPE_INTEGER_MENU: K_LOG_INFO("    Type: V4L2_CTRL_TYPE_INTEGER_MENU.\n"); break;
            case V4L2_CTRL_TYPE_BITMASK: K_LOG_INFO("    Type: V4L2_CTRL_TYPE_BITMASK.\n"); break;
            case V4L2_CTRL_TYPE_BUTTON: K_LOG_INFO("    Type: V4L2_CTRL_TYPE_BUTTON.\n"); break;
            case V4L2_CTRL_TYPE_INTEGER64: K_LOG_INFO("    Type: V4L2_CTRL_TYPE_INTEGER64.\n"); break;
            case V4L2_CTRL_TYPE_STRING: K_LOG_INFO("    Type: V4L2_CTRL_TYPE_STRING.\n"); break;
            case V4L2_CTRL_TYPE_CTRL_CLASS: K_LOG_INFO("    Type: V4L2_CTRL_TYPE_CTRL_CLASS.\n"); break;
            case V4L2_CTRL_TYPE_U8: K_LOG_INFO("    Type: V4L2_CTRL_TYPE_U8.\n"); break;
            case V4L2_CTRL_TYPE_U16: K_LOG_INFO("    Type: V4L2_CTRL_TYPE_U16.\n"); break;
            case V4L2_CTRL_TYPE_U32: K_LOG_INFO("    Type: V4L2_CTRL_TYPE_U32.\n"); break;
            default: K_LOG_INFO("    Type: unknown.\n"); break;
        }
        //flags
        if((ctrl->flags & V4L2_CTRL_FLAG_DISABLED)) { K_LOG_INFO("    Flag: V4L2_CTRL_FLAG_DISABLED.\n"); }
        if((ctrl->flags & V4L2_CTRL_FLAG_GRABBED)) { K_LOG_INFO("    Flag: V4L2_CTRL_FLAG_GRABBED.\n"); }
        if((ctrl->flags & V4L2_CTRL_FLAG_READ_ONLY)) { K_LOG_INFO("    Flag: V4L2_CTRL_FLAG_READ_ONLY.\n"); }
        if((ctrl->flags & V4L2_CTRL_FLAG_UPDATE)) { K_LOG_INFO("    Flag: V4L2_CTRL_FLAG_UPDATE.\n"); }
        if((ctrl->flags & V4L2_CTRL_FLAG_INACTIVE)) { K_LOG_INFO("    Flag: V4L2_CTRL_FLAG_INACTIVE.\n"); }
        if((ctrl->flags & V4L2_CTRL_FLAG_SLIDER)) { K_LOG_INFO("    Flag: V4L2_CTRL_FLAG_SLIDER.\n"); }
        if((ctrl->flags & V4L2_CTRL_FLAG_WRITE_ONLY)) { K_LOG_INFO("    Flag: V4L2_CTRL_FLAG_WRITE_ONLY.\n"); }
        if((ctrl->flags & V4L2_CTRL_FLAG_VOLATILE)) { K_LOG_INFO("    Flag: V4L2_CTRL_FLAG_VOLATILE.\n"); }
        if((ctrl->flags & V4L2_CTRL_FLAG_HAS_PAYLOAD)) { K_LOG_INFO("    Flag: V4L2_CTRL_FLAG_HAS_PAYLOAD.\n"); }
        if((ctrl->flags & V4L2_CTRL_FLAG_EXECUTE_ON_WRITE)) { K_LOG_INFO("    Flag: V4L2_CTRL_FLAG_EXECUTE_ON_WRITE.\n"); }
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
                        K_LOG_INFO("    Menu #%d: '%s' = %lld.\n", (i2 + 1), mnu.name, mnu.value);
                    } else {
                        K_LOG_INFO("    Menu #%d: '%s'.\n", (i2 + 1), mnu.name);
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

int VideoFrameStates_empty(STVideoFrameStates* obj){
    int i; for(i = 0; i < obj->use; i++){
        STVideoFrameState* st = &obj->arr[i];
        VideoFrameState_release(st);
    }
    obj->use = 0;
    return 0;
}


//STVideoFrame
//In H264, an Access unit allways produces an output frame.
//IDR = Instantaneous Decoding Refresh

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
    memset(&obj->accessUnit, 0, sizeof(obj->accessUnit)); //counts of NALs contained by this frame, by type (32 max)
    //
    if(0 != VideoFrameState_reset(&obj->state)){
        K_LOG_INFO("VideoFrame_reset, VideoFrameState_reset failed.\n");
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
            K_LOG_INFO("VideoFrame_prepareForFill, malloc failed.\n");
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
        K_LOG_VERBOSE("VideoFrame_prepareForFill, buff growth to %dKB.\n", (obj->buff.sz / 1024));
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

int VideoFrame_getNalsCount(const STVideoFrame* obj){
    return  obj->accessUnit.nalsCountPerType[0]
    + obj->accessUnit.nalsCountPerType[1]
    + obj->accessUnit.nalsCountPerType[2]
    + obj->accessUnit.nalsCountPerType[3]
    + obj->accessUnit.nalsCountPerType[4]
    + obj->accessUnit.nalsCountPerType[5]
    + obj->accessUnit.nalsCountPerType[6]
    + obj->accessUnit.nalsCountPerType[7]
    + obj->accessUnit.nalsCountPerType[8]
    + obj->accessUnit.nalsCountPerType[9]
    + obj->accessUnit.nalsCountPerType[10]
    + obj->accessUnit.nalsCountPerType[11]
    + obj->accessUnit.nalsCountPerType[12]
    + obj->accessUnit.nalsCountPerType[13]
    + obj->accessUnit.nalsCountPerType[14]
    + obj->accessUnit.nalsCountPerType[15]
    + obj->accessUnit.nalsCountPerType[16]
    + obj->accessUnit.nalsCountPerType[17]
    + obj->accessUnit.nalsCountPerType[18]
    + obj->accessUnit.nalsCountPerType[19]
    + obj->accessUnit.nalsCountPerType[20]
    + obj->accessUnit.nalsCountPerType[21]
    + obj->accessUnit.nalsCountPerType[22]
    + obj->accessUnit.nalsCountPerType[23]
    + obj->accessUnit.nalsCountPerType[24]
    + obj->accessUnit.nalsCountPerType[25]
    + obj->accessUnit.nalsCountPerType[26]
    + obj->accessUnit.nalsCountPerType[27]
    + obj->accessUnit.nalsCountPerType[28]
    + obj->accessUnit.nalsCountPerType[29]
    + obj->accessUnit.nalsCountPerType[30]
    + obj->accessUnit.nalsCountPerType[31];
}

int VideoFrame_getNalsCountOfGrp(const STVideoFrame* obj, const ENNalTypeGrp grp){
    int r = 0;
    int i; for(i = 0; i < sizeof(obj->accessUnit.nalsCountPerType) / sizeof(obj->accessUnit.nalsCountPerType[0]); i++){
        if(obj->accessUnit.nalsCountPerType[i] > 0 && _naluDefs[i].grp == grp){
            r += obj->accessUnit.nalsCountPerType[i];
        }
    }
    return r;
}


int VideoFrame_setAccessUnitDelimiterFound(STVideoFrame* obj, const int primary_pic_type){
    int r = 0;
    if(obj->accessUnit.delimeter.isPresent){
        K_LOG_ERROR("VideoFrame, already has an access-unit-delimiter.\n");
        r = -1;
    } else {
        obj->accessUnit.delimeter.isPresent = 1;
        obj->accessUnit.delimeter.primary_pic_type = primary_pic_type;
        switch(primary_pic_type){
            case 0:
                obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[2] = 1;
                obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[7] = 1;
                break;
            case 1:
                obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[0] = 1;
                obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[2] = 1;
                obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[5] = 1;
                obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[7] = 1;
                break;
            case 2:
                obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[0] = 1;
                obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[1] = 1;
                obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[2] = 1;
                obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[5] = 1;
                obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[6] = 1;
                obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[7] = 1;
                break;
            case 3:
                obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[4] = 1;
                obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[9] = 1;
                break;
            case 4:
                obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[3] = 1;
                obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[4] = 1;
                obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[8] = 1;
                obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[9] = 1;
                break;
            case 5:
                obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[2] = 1;
                obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[4] = 1;
                obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[7] = 1;
                obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[9] = 1;
                break;
            case 6:
                obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[0] = 1;
                obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[2] = 1;
                obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[3] = 1;
                obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[4] = 1;
                obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[5] = 1;
                obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[7] = 1;
                obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[8] = 1;
                obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[9] = 1;
                break;
            case 7:
                obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[0] = 1;
                obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[1] = 1;
                obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[2] = 1;
                obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[3] = 1;
                obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[4] = 1;
                obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[5] = 1;
                obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[6] = 1;
                obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[7] = 1;
                obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[8] = 1;
                obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[9] = 1;
                break;
            default:
                obj->accessUnit.isInvalid = 1;
                r = -1;
                K_ASSERT(0);
                break;
        }
        /*K_LOG_INFO("StreamContext, Frame access-unit-delimiter primary_pic_type(%d, types:%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s).\n"
                   , obj->accessUnit.delimeter.primary_pic_type
                   , obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[0] ? " 0" : ""
                   , obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[1] ? " 1" : ""
                   , obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[2] ? " 2" : ""
                   , obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[3] ? " 3" : ""
                   , obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[4] ? " 4" : ""
                   , obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[5] ? " 5" : ""
                   , obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[6] ? " 6" : ""
                   , obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[7] ? " 7" : ""
                   , obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[8] ? " 8" : ""
                   , obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[9] ? " 9" : ""
                   , obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[10] ? " 10" : ""
                   , obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[11] ? " 11" : ""
                   , obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[12] ? " 12" : ""
                   , obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[13] ? " 13" : ""
                   , obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[14] ? " 14" : ""
                   , obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[15] ? " 15" : ""
                   , obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[16] ? " 16" : ""
                   , obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[17] ? " 17" : ""
                   , obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[18] ? " 18" : ""
                   , obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[19] ? " 19" : ""
                   , obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[20] ? " 20" : ""
                   , obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[21] ? " 21" : ""
                   , obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[22] ? " 22" : ""
                   , obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[23] ? " 23" : ""
                   , obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[24] ? " 24" : ""
                   , obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[25] ? " 25" : ""
                   , obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[26] ? " 26" : ""
                   , obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[27] ? " 27" : ""
                   , obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[28] ? " 28" : ""
                   , obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[29] ? " 29" : ""
                   , obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[30] ? " 30" : ""
                   , obj->accessUnit.delimeter.slicesAllowedPrimaryPicturePerType[31] ? " 31" : ""
        );*/
    }
    return r;
}

//the buffer is now owned outside the frame, NULLify
int VideoFrame_resignToBuffer(STVideoFrame* obj){
    int r = 0;
    
    return r;
}

//STVideoFrames

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
            f->state.iSeq = obj->iSeqPullNext++;
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
            f->state.iSeq = obj->iSeqPullNext++;
            //
            *dst = f;
            return 0;
        }
    }
    return -1;
}

//get from the left

int VideoFrames_getFramesForReadCount(STVideoFrames* obj){ //peek from the left
    return obj->use;
}

int VideoFrames_pullFrameForRead(STVideoFrames* obj, STVideoFrame** dst){
    if(dst != NULL && obj->use > 0){
        //pull first element
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
    obj->iSeqPushNext++;
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
            K_LOG_INFO("Framebuff, layoutSet, malloc failed.\n");
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
            K_LOG_VERBOSE("FbLayoutRow_fillGaps, added left rect.\n");
        }
        //black area on the right
        if((last.x + last.width) < widthMax){
            FbLayoutRow_add(obj, 0, (last.x + last.width), 0, widthMax - (last.x + last.width), obj->height);
            K_LOG_VERBOSE("FbLayoutRow_fillGaps, added right rect.\n");
        }
    }
    //black areas in current rects
    {
        int i; for(i = (int)obj->rectsUse - 1; i >= 0; i--){ //reverse order because array will grow (ignore new records).
            const STFbRect rect = obj->rects[i].rect; //local copy (array will be modified)
            //black area on the top
            if(rect.y > 0){
                FbLayoutRow_add(obj, 0, rect.x, 0, rect.width, rect.y);
                K_LOG_VERBOSE("FbLayoutRow_fillGaps, added top rect.\n");
            }
            //black area on the bottom
            if((rect.y + rect.height) < obj->height){
                FbLayoutRow_add(obj, 0, rect.x, rect.y + rect.height, rect.width, (obj->height - (rect.y + rect.height)));
                K_LOG_VERBOSE("FbLayoutRow_fillGaps, added bottom rect.\n");
            }
        }
    }
    return 0;
}

//STFramebuffPtr

void FramebuffPtr_init(STFramebuffPtr* obj){
    memset(obj, 0, sizeof(*obj));
}

void FramebuffPtr_release(STFramebuffPtr* obj){
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
    //
    if(obj->fd >= 0){
        close(obj->fd);
        obj->fd = -1;
    }
}

//

int Framebuff_open(STFramebuff* obj, const char* device){
    int r = -1;
    int fd = open(device, O_RDWR);
    if(fd < 0){
        K_LOG_ERROR("Framebuff, open failed: '%s'.\n", device);
    } else {
        //query variables
        struct fb_var_screeninfo vinfo;
        struct fb_fix_screeninfo finfo;
        memset(&vinfo, 0, sizeof(vinfo));
        memset(&finfo, 0, sizeof(finfo));
        if(0 != ioctl(fd, FBIOGET_VSCREENINFO, &vinfo)) {
            K_LOG_ERROR("Framebuff, get variable info failed: '%s'.\n", device);
        } else if(0 != ioctl(fd, FBIOGET_FSCREENINFO, &finfo)) {
            K_LOG_ERROR("Framebuff, get fixed info failed: '%s'.\n", device);
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
            K_LOG_INFO("Framebuff, opened: '%s'.\n", device);
            K_LOG_INFO("Framebuff, fixed info:\n");
            K_LOG_INFO("           smem_start: %u.\n", finfo.smem_start);
            K_LOG_INFO("             smem_len: %u.\n", finfo.smem_len);
            K_LOG_INFO("                 type: %u ('%s').\n", finfo.type, finfo.type == FB_TYPE_PACKED_PIXELS ? "FB_TYPE_PACKED_PIXELS" : finfo.type == FB_TYPE_PLANES ? "FB_TYPE_PLANES" : finfo.type == FB_TYPE_INTERLEAVED_PLANES ? "FB_TYPE_INTERLEAVED_PLANES" : finfo.type == FB_TYPE_FOURCC ? "FB_TYPE_FOURCC" : "UNKNOWN_STR");
            K_LOG_INFO("             type_aux: %u ('%s').\n", finfo.type_aux, finfo.type_aux == FB_TYPE_PACKED_PIXELS ? "FB_TYPE_PACKED_PIXELS" : finfo.type_aux == FB_TYPE_PLANES ? "FB_TYPE_PLANES" : finfo.type_aux == FB_TYPE_INTERLEAVED_PLANES ? "FB_TYPE_INTERLEAVED_PLANES" : finfo.type_aux == FB_TYPE_FOURCC ? "FB_TYPE_FOURCC" : "UNKNOWN_STR");
            K_LOG_INFO("               visual: %u ('%s').\n", finfo.visual, finfo.visual == FB_VISUAL_MONO01 ? "FB_VISUAL_MONO01" : finfo.visual == FB_VISUAL_MONO10 ? "FB_VISUAL_MONO10" : finfo.visual == FB_VISUAL_TRUECOLOR ? "FB_VISUAL_TRUECOLOR" : finfo.visual == FB_VISUAL_PSEUDOCOLOR ? "FB_VISUAL_PSEUDOCOLOR" : finfo.visual == FB_VISUAL_STATIC_PSEUDOCOLOR ? "FB_VISUAL_STATIC_PSEUDOCOLOR" : finfo.visual == FB_VISUAL_DIRECTCOLOR ? "FB_VISUAL_DIRECTCOLOR" : finfo.visual == FB_VISUAL_FOURCC ? "FB_VISUAL_FOURCC" : "UNKNOWN_STR");
            K_LOG_INFO("             xpanstep: %u.\n", finfo.xpanstep);
            K_LOG_INFO("             ypanstep: %u.\n", finfo.ypanstep);
            K_LOG_INFO("            ywrapstep: %u.\n", finfo.ywrapstep);
            K_LOG_INFO("          line_length: %u.\n", finfo.line_length);
            K_LOG_INFO("           mmio_start: %u.\n", finfo.mmio_start);
            K_LOG_INFO("             mmio_len: %u.\n", finfo.mmio_len);
            K_LOG_INFO("                accel: %u.\n", finfo.accel);
            K_LOG_INFO("         capabilities: %u%s.\n", finfo.capabilities, (finfo.capabilities & FB_CAP_FOURCC) ? " FB_CAP_FOURCC" : "");
            //
            K_LOG_INFO("Framebuff, variable info:\n");
            K_LOG_INFO("                 xres: %u.\n", vinfo.xres);
            K_LOG_INFO("                 yres: %u.\n", vinfo.yres);
            K_LOG_INFO("         xres_virtual: %u.\n", vinfo.xres_virtual);
            K_LOG_INFO("         yres_virtual: %u.\n", vinfo.yres_virtual);
            K_LOG_INFO("              xoffset: %u.\n", vinfo.xoffset);
            K_LOG_INFO("              yoffset: %u.\n", vinfo.yoffset);
            K_LOG_INFO("       bits_per_pixel: %u.\n", vinfo.bits_per_pixel);
            K_LOG_INFO("            grayscale: %u (%s).\n", vinfo.grayscale, vinfo.grayscale == 0 ? "COLOR" : vinfo.grayscale == 1 ? "GRAYSCALE" : "FOURCC");
            K_LOG_INFO("                  red: %u, +%u, %s.\n", vinfo.red.offset, vinfo.red.length, vinfo.red.msb_right ? "msb_right" : "msb_left");
            K_LOG_INFO("                green: %u, +%u, %s.\n", vinfo.green.offset, vinfo.green.length, vinfo.green.msb_right ? "msb_right" : "msb_left");
            K_LOG_INFO("                 blue: %u, +%u, %s.\n", vinfo.blue.offset, vinfo.blue.length, vinfo.blue.msb_right ? "msb_right" : "msb_left");
            K_LOG_INFO("               transp: %u, +%u, %s.\n", vinfo.transp.offset, vinfo.transp.length, vinfo.transp.msb_right ? "msb_right" : "msb_left");
            K_LOG_INFO("               nonstd: %u.\n", vinfo.nonstd);
            K_LOG_INFO("             activate: %u.\n", vinfo.activate);
            K_LOG_INFO("               height: %u mm.\n", vinfo.height);
            K_LOG_INFO("                width: %u mm.\n", vinfo.width);
            K_LOG_INFO("             pixclock: %u pico-secs.\n", vinfo.pixclock);
            K_LOG_INFO("          left_margin: %u pixclocks.\n", vinfo.left_margin);
            K_LOG_INFO("         right_margin: %u pixclocks.\n", vinfo.right_margin);
            K_LOG_INFO("         upper_margin: %u pixclocks.\n", vinfo.upper_margin);
            K_LOG_INFO("         lower_margin: %u pixclocks.\n", vinfo.lower_margin);
            K_LOG_INFO("            hsync_len: %u pixclocks.\n", vinfo.hsync_len);
            K_LOG_INFO("            vsync_len: %u pixclocks.\n", vinfo.vsync_len);
            K_LOG_INFO("                 sync: %u.\n", vinfo.sync);
            K_LOG_INFO("                vmode: %u.\n", vinfo.vmode);
            K_LOG_INFO("               rotate: %u deg.\n", vinfo.rotate);
            K_LOG_INFO("           colorspace: %u.\n", vinfo.colorspace);
            //
            if(pixFmt == 0){
                K_LOG_ERROR("Framebuff, unsupported pixfmt: '%s' (add this case to source code!).\n", device);
            } else if(MAP_FAILED == (ptr = (unsigned char*)mmap(0, finfo.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0))){
                K_LOG_ERROR("Framebuff, mmap failed.\n", device);
            } else if(!(offPtr = malloc(finfo.smem_len))){
                K_LOG_ERROR("Framebuff, malloc for offscreen buffer failed.\n", device);
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
                            memset(obj->blackLine, 0, obj->bytesPerLn);
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
                K_LOG_ERROR("Framebuff, bitblit, src bytesPerLn is not 32-bits-aligned.\n");
            } else if((obj->bytesPerLn % 4) != 0){
                K_LOG_ERROR("Framebuff, bitblit, buffer bytesPerLn is not 32-bits-aligned.\n");
            } else {
                //copy lines
                if(pos.x == 0 && srcRect.x == 0 && srcRect.width == obj->width && srcPixs->bytesPerLn == obj->bytesPerLn){
                    //Fast-copy (if src-lines and dst-lines match)
                    unsigned char* srcLn = &srcPixs->dataPtr[(srcPixs->bytesPerLn * y) + (bytesPerPx * srcRect.x)];
                    unsigned char* dstLn = &dst->ptr[(obj->bytesPerLn * pos.y) + (bytesPerPx * pos.x)];
                    int copyLen = obj->bytesPerLn * srcRect.height;
                    //K_LOG_INFO("Framebuff, bitblit, optimized copy of %d lines.\n", srcRect.height);
                    K_ASSERT(dstLn >= dst->ptr && (dstLn + copyLen) <= (dst->ptr + dst->ptrSz)) //must be inside the destination range
                    if(copyLen > 0){
                        memcpy(dstLn, srcLn, copyLen);
                    }
                } else {
                    //Copy line per line
                    while(y < yAfterEnd){
                        unsigned char* srcLn = &srcPixs->dataPtr[(srcPixs->bytesPerLn * y) + (bytesPerPx * srcRect.x)];
                        unsigned char* dstLn = &dst->ptr[(obj->bytesPerLn * pos.y) + (bytesPerPx * pos.x)];
                        int copyLen = bytesPerPx * srcRect.width;
                        K_ASSERT(dstLn >= dst->ptr && (dstLn + copyLen) <= (dst->ptr + dst->ptrSz)) //must be inside the destination range
                        if(copyLen > 0){
                            memcpy(dstLn, srcLn, copyLen);
                        }
                        y++; pos.y++;
                    }
                }
            }
        }
        r = 0;
    }
    return r;
}

int Framebuff_drawRowsBuildPlan(STFramebuff* obj, STFramebuffPtr* dst, STFramebuffDrawRect* rects, const int rectsUse, STFramebuffDrawLine* lines, int linesSz, int* linesUse){
    int r = 0;
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
                    if(*linesUse < linesSz){
                        STFramebuffDrawLine* ln = &lines[*linesUse];
                        ln->dst = &dst->ptr[(obj->bytesPerLn * rect->posCur.y) + (bytesPerPx * rect->posCur.x)];
                        if(rect->plane != NULL){
                            ln->src = &rect->plane->dataPtr[(rect->plane->bytesPerLn * rect->srcRectY) + (bytesPerPx * rect->srcRectX)];
                        } else {
                            ln->src = obj->blackLine;
                        }
                        ln->sz  = bytesPerPx * rect->srcRectWidth;
                        K_ASSERT(ln->dst >= dst->ptr && (ln->dst + ln->sz) <= (dst->ptr + dst->ptrSz)) //must be inside the destination range
                    } else {
                        r = -1;
                    }
                    lnFound = 1;
                    (*linesUse)++;
                    //next line
                    rect->posCur.y++;
                    rect->srcRectY++;
                }
                //next rect
                rect++;
            }
            //next line
            yDest++;
        } while(lnFound);
    }
    return r;
}

//STFramebuffsGrpFb

void FramebuffsGrpFb_init(STFramebuffsGrpFb* obj){
    memset(obj, 0, sizeof(*obj));
}

void FramebuffsGrpFb_release(STFramebuffsGrpFb* obj){
    obj->fb = NULL;
}

//STFramebuffsGrp
//fbs are grouped by 'pixFmt'

void FramebuffsGrp_init(STFramebuffsGrp* obj){
    memset(obj, 0, sizeof(*obj));
}

void FramebuffsGrp_release(STFramebuffsGrp* obj){
    //streams
    {
        if(obj->streams.arr != NULL){
            free(obj->streams.arr);
            obj->streams.arr = NULL;
        }
        obj->streams.use = 0;
        obj->streams.sz = 0;
    }
    //fbs
    {
        if(obj->fbs.arr != NULL){
            int i; for(i = 0; i < obj->fbs.use; i++){
                STFramebuffsGrpFb* gfb = &obj->fbs.arr[i];
                FramebuffsGrpFb_release(gfb);
            }
            free(obj->fbs.arr);
            obj->fbs.arr = NULL;
        }
        obj->fbs.use = 0;
        obj->fbs.sz = 0;
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
}

int FramebuffsGrp_addFb(STFramebuffsGrp* obj, STFramebuff* fb, const ENFramebuffsGrpFbLocation location, const int x, const int y){
    int r = -1;
    if(location >= 0 && location < ENFramebuffsGrpFbLocation_Count){
        //resize
        while(obj->fbs.use >= obj->fbs.sz){
            const int szN = obj->fbs.sz + 8;
            STFramebuffsGrpFb* arrN = (STFramebuffsGrpFb*)malloc(sizeof(obj->fbs.arr[0]) * szN);
            if(arrN == NULL){
                break;
            } else {
                if(obj->fbs.arr != NULL){
                    if(obj->fbs.use > 0){
                        memcpy(arrN, obj->fbs.arr, sizeof(obj->fbs.arr[0]) * obj->fbs.use);
                    }
                    free(obj->fbs.arr);
                }
                obj->fbs.arr = arrN;
                obj->fbs.sz = szN;
            }
        }
        //add
        if(obj->fbs.use < obj->fbs.sz){
            STFramebuffsGrpFb* gfb = &obj->fbs.arr[obj->fbs.use++];
            FramebuffsGrpFb_init(gfb);
            gfb->fb = fb;
            switch (location) {
                case ENFramebuffsGrpFbLocation_Free:
                    gfb->x = x;
                    gfb->y = y;
                    gfb->cfg.location = location;
                    r = 0;
                    break;
                case ENFramebuffsGrpFbLocation_Right:
                    gfb->x = obj->xRightNxt;
                    gfb->cfg.location = location;
                    r = 0;
                    break;
                case ENFramebuffsGrpFbLocation_Bottom:
                    gfb->y = obj->yBottomNxt;
                    gfb->cfg.location = location;
                    r = 0;
                    break;
                case ENFramebuffsGrpFbLocation_Left:
                    gfb->x = obj->xLeft - fb->width;
                    gfb->cfg.location = location;
                    r = 0;
                    break;
                case ENFramebuffsGrpFbLocation_Top:
                    gfb->y = obj->yTop - fb->height;
                    gfb->cfg.location = location;
                    r = 0;
                    break;
                default:
                    K_LOG_INFO("Error, FramebuffsGrp_addFb, unsupported 'location' value.\n");
                    FramebuffsGrpFb_release(gfb);
                    obj->fbs.use--;
                    break;
            }
            //grow grp
            if(r == 0){
                if(obj->xLeft > gfb->x) obj->xLeft = gfb->x;
                if(obj->yTop > gfb->y) obj->yTop = gfb->y;
                if(obj->xRightNxt < (gfb->x + fb->width)) obj->xRightNxt = (gfb->x + fb->width);
                if(obj->yBottomNxt < (gfb->y + fb->height)) obj->yBottomNxt = (gfb->y + fb->height);
            }
        }
    }
    return r;
}

int FramebuffsGrp_addStream(STFramebuffsGrp* obj, STStreamContext* ctx){
    int r = -1;
    //resize
    while(obj->streams.use >= obj->streams.sz){
        const int szN = obj->streams.sz + 8;
        STStreamContext** arrN = (STStreamContext**)malloc(sizeof(obj->streams.arr[0]) * szN);
        if(arrN == NULL){
            break;
        } else {
            if(obj->streams.arr != NULL){
                if(obj->streams.use > 0){
                    memcpy(arrN, obj->streams.arr, sizeof(obj->streams.arr[0]) * obj->streams.use);
                }
                free(obj->streams.arr);
            }
            obj->streams.arr = arrN;
            obj->streams.sz = szN;
        }
    }
    //add
    if(obj->streams.use < obj->streams.sz){
        obj->streams.arr[obj->streams.use++] = ctx;
        r = 0;
    }
    return r;
}

//resets 'isFound'
int FramebuffsGrp_layoutStart(STFramebuffsGrp* obj){
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
int FramebuffsGrp_layoutEnd(STFramebuffsGrp* obj){
    if(obj->layout.rows.use > 0){
        STFbLayoutRow* row = &obj->layout.rows.arr[obj->layout.rows.use - 1];
        if(row->rectsUse == 0){
            //remove empty row
            FbLayoutRow_release(row);
            obj->layout.rows.use--;
        } else {
            //close row
            FbLayoutRow_fillGaps(row, obj->xRightNxt - obj->xLeft);
            obj->layout.rows.rectsCount += row->rectsUse;
            if(obj->layout.width < row->width) obj->layout.width = row->width;
            if(obj->layout.height < (row->yTop + row->height)) obj->layout.height = (row->yTop + row->height);
        }
    }
    return 0;
}

//updates or add the stream, flags it as 'isFound'
int FramebuffsGrp_layoutAdd(STFramebuffsGrp* obj, int streamId, const STFbSize size){
    int r = -1;
    //current row
    if(obj->layout.rows.use > 0){
        STFbLayoutRow* row = &obj->layout.rows.arr[obj->layout.rows.use - 1];
        if(row->rectsUse == 0 || (row->width + size.width) <= (obj->xRightNxt - obj->xLeft)){
            //add
            FbLayoutRow_add(row, streamId, row->width, 0, size.width, size.height);
        } else {
            //close previous row
            FbLayoutRow_fillGaps(row, obj->xRightNxt - obj->xLeft);
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
int FramebuffsGrp_layoutFindStreamId(STFramebuffsGrp* obj, int streamId){
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

int FramebuffsGrp_layoutAnimTick(STFramebuffsGrp* obj, const int ms, struct STPlayer_* plyr, const int msMinToDoDrawPlanNextPos, const int doDrawPlanIfAnimating){
    int r = 0;
    //layout
    if(ms > 0){
        const int msWaitBefore = obj->layout.anim.msWait;
        const int iRowFirstBefore = obj->layout.anim.iRowFirst;
        //wait
        {
            if(obj->layout.anim.msWait <= ms){
                obj->layout.anim.msWait = 0;
            } else {
                obj->layout.anim.msWait -= ms;
            }
        }
        if(obj->layout.anim.msWait > 0){
            if(obj->layout.anim.msWait < msMinToDoDrawPlanNextPos){
                
            }
        } else if(obj->layout.anim.msWait <= 0){
            //row animation
            if(obj->layout.rows.use <= 0 || obj->layout.height <= 0){
                obj->layout.anim.iRowFirst  = 0;
                obj->layout.anim.yOffset    = 0;
                obj->layout.anim.msWait     = (obj->cfg.animSecsWaits * 1000);
                //flag to refresh
                obj->isSynced               = 0;
            } else {
                //move rows
                STFbLayoutRow* row          = &obj->layout.rows.arr[obj->layout.anim.iRowFirst % obj->layout.rows.use];
                int pxMoveV                 = (((obj->yBottomNxt - obj->yTop) * 100 / 100) * ms / 1000); //100% per sec
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
                        obj->isSynced               = 0;
                    } else if(doDrawPlanIfAnimating){
                        //add animation finals-rect to drawPlan
                        {
                            int rectsUse = 0;
                            if(0 != FramebuffsGrp_drawGetRects(obj, plyr, (yOffsetDst % obj->layout.height), NULL, 0, &rectsUse)){
                                //expected to fail, just calculating and updating hitsCount
                            }
                        }
                    }
                }
            }
        }
        //if an animation-start is close, or is the animation first tick
        //add animation next-finals-rect to drawPlan (prepare for incoming animation)
        if(msWaitBefore > 0 && obj->layout.anim.msWait < msMinToDoDrawPlanNextPos){
            K_LOG_VERBOSE("FramebuffsGrp, %ums to anim, considering rects from next position.\n", obj->layout.anim.msWait);
            if(obj->layout.rows.use > 0 && obj->layout.height > 0){
                int rectsUse = 0;
                STFbLayoutRow* row = &obj->layout.rows.arr[(iRowFirstBefore + 1) % obj->layout.rows.use];
                int yOffsetDst = -(row->yTop);
                while(yOffsetDst > obj->layout.anim.yOffset) yOffsetDst -= obj->layout.height;
                if(0 != FramebuffsGrp_drawGetRects(obj, plyr, (yOffsetDst % obj->layout.height), NULL, 0, &rectsUse)){
                    //expected to fail, just calculating and updating hitsCount
                }
            }
        }
    }
    return r;
}

int FramebuffsGrp_drawGetRects(STFramebuffsGrp* obj, struct STPlayer_* plyr, const int yOffset, STFramebuffDrawRect* rects, int rectsSz, int* dstRectsUse){
    int r = 0;
    int j; for(j = 0; j < obj->fbs.use; j++){
        STFramebuffsGrpFb* gfb = &obj->fbs.arr[j];
        STFramebuff* fb = gfb->fb;
        if(fb != NULL){
            STFbRect layRect; //fb's rect relative to layout
            memset(&layRect, 0, sizeof(layRect));
            layRect.x       = gfb->x - obj->xLeft;
            layRect.y       = gfb->y - obj->yTop;
            layRect.width   = fb->width;
            layRect.height  = fb->height;
            //analyze rows
            if(
               //fb is inside grp's layout
               !(layRect.x >= obj->layout.width || layRect.y >= obj->layout.height || (layRect.x + layRect.width) < 0 || (layRect.y + layRect.height) < 0)
               //fb is not empty
               && layRect.width > 0 && layRect.height > 0
               //layout has rows and rects
               && obj->layout.rows.use > 0 && obj->layout.rows.rectsCount > 0
               )
            {
                //add all rects to be drawn
                int rowsAddedCount = 0;
                int yTop = yOffset;
                K_LOG_VERBOSE("Draw planed, yOffset(%d).\n", yOffset);
                while(1){
                    int yTopBefore = yTop;
                    int i2; for(i2 = 0; i2 < obj->layout.rows.use; i2++){
                        STFbLayoutRow* row = &obj->layout.rows.arr[i2];
                        //fb is inside rows's layout?
                        if(layRect.x >= row->width || layRect.y >= (yTop + row->height) || (layRect.x + layRect.width) < 0 || (layRect.y + layRect.height) < yTop){
                            //skip row
                        } else {
                            //analyze row's rects
                            int rowXPrev = -1, yTopPrev = yTop;
                            int i; for(i = 0; i < row->rectsUse; i++){
                                STFbLayoutRect* lrRect = &row->rects[i];
                                if(layRect.x >= (lrRect->rect.x + lrRect->rect.width) || layRect.y >= (yTop + lrRect->rect.y + lrRect->rect.height) || (layRect.x + layRect.width) < lrRect->rect.x || (layRect.y + layRect.height) < (yTop + lrRect->rect.y)){
                                    //skip rect
                                } else {
                                    //add rect
                                    int rectAdded = 0;
                                    K_ASSERT(rowXPrev <= lrRect->rect.x);
                                    rowXPrev = lrRect->rect.x;
                                    //add stream
                                    if(lrRect->streamId > 0){
                                        int j; for(j = 0; j < plyr->streams.arrUse; j++){
                                            STStreamContext* ctx = plyr->streams.arr[j];
                                            if(ctx->streamId == lrRect->streamId){
                                                if(ctx->drawPlan.lastCompRect.width > 0 && ctx->drawPlan.lastCompRect.height > 0 && ctx->drawPlan.lastPixelformat == fb->pixFmt){
                                                    //render last buffer
                                                    STPlane* plane = NULL;
                                                    if(ctx->dec.fd >= 0){
                                                        STBuffer* buff = (ctx->dec.dst.isLastDequeuedCloned ? &ctx->dec.dst.lastDequeuedClone : ctx->dec.dst.lastDequeued);
                                                        if(buff != NULL && buff->planesSz > 0){
                                                            plane = &buff->planes[0];
                                                        }
                                                    }
                                                    //Add rect
                                                    {
                                                        STFbRect srcRect = ctx->drawPlan.lastCompRect;
                                                        STFbPos pos;
                                                        pos.x = (lrRect->rect.x - layRect.x);
                                                        pos.y = (yTop + lrRect->rect.y - layRect.y);
                                                        //
                                                        if(0 != Framebuff_validateRect(fb, pos, &pos, srcRect, &srcRect)){
                                                            K_LOG_ERROR("StreamContext, validateRect failed.\n");
                                                        } else if(srcRect.width > 0 && srcRect.height > 0){
                                                            if(rects != NULL && *dstRectsUse < rectsSz){
                                                                STFramebuffDrawRect* rect = &rects[*dstRectsUse];
                                                                //
                                                                K_LOG_VERBOSE("Stream-rect-added row(%d) pos(%d, %d) srcRect(%d, %d)-(+%d, +%d).\n", i2, pos.x, pos.y, srcRect.x, srcRect.y, srcRect.width, srcRect.height);
                                                                //
                                                                rect->iRow      = rowsAddedCount;
                                                                rect->fb        = fb;
                                                                rect->plane     = plane;
                                                                rect->posCur    = pos;
                                                                rect->srcRectX  = srcRect.x;
                                                                rect->srcRectWidth = srcRect.width;
                                                                rect->srcRectY  = srcRect.y;
                                                                rect->srcRectYAfterEnd = srcRect.y + srcRect.height;
                                                            } else {
                                                                r = -1;
                                                            }
                                                            //increase stream-ctx hitsCount (used to start/stop decoder)
                                                            ctx->drawPlan.hitsCount++;
                                                            //
                                                            rectAdded = 1;
                                                            (*dstRectsUse)++;
                                                        } else {
                                                            K_LOG_VERBOSE("Stream-rect-ignored row(%d) pos(%d, %d) srcRect(%d, %d)-(+%d, +%d).\n", i2, pos.x, pos.y, srcRect.x, srcRect.y, srcRect.width, srcRect.height);
                                                        }
                                                    }
                                                }
                                                break;
                                            }
                                        }
                                    }
                                    //add black rect at row
                                    if(!rectAdded){
                                        STFbRect srcRect;
                                        STFbPos pos;
                                        pos.x = lrRect->rect.x - layRect.x;
                                        pos.y = yTop + lrRect->rect.y - layRect.y;
                                        srcRect.x = 0;
                                        srcRect.y = 0;
                                        srcRect.width = lrRect->rect.width;
                                        srcRect.height = lrRect->rect.height;
                                        if(0 != Framebuff_validateRect(fb, pos, &pos, srcRect, &srcRect)){
                                            K_LOG_ERROR("StreamContext, validateRect failed.\n");
                                        } else if(srcRect.width > 0 && srcRect.height > 0){
                                            if(rects != NULL && *dstRectsUse < rectsSz){
                                                STFramebuffDrawRect* rect = &rects[*dstRectsUse];
                                                //
                                                K_LOG_VERBOSE("Stream-rect-added-black row(%d) pos(%d, %d) srcRect(%d, %d)-(+%d, +%d).\n", i2, pos.x, pos.y, srcRect.x, srcRect.y, srcRect.width, srcRect.height);
                                                //
                                                rect->iRow      = rowsAddedCount;
                                                rect->fb        = fb;
                                                rect->plane     = NULL; //NULL means: 'use the blackLine array'
                                                rect->posCur    = pos;
                                                rect->srcRectX  = srcRect.x;
                                                rect->srcRectWidth = srcRect.width;
                                                rect->srcRectY  = srcRect.y;
                                                rect->srcRectYAfterEnd = srcRect.y + srcRect.height;
                                            } else {
                                                r = -1;
                                            }
                                            //
                                            rectAdded = 1;
                                            (*dstRectsUse)++;
                                        } else {
                                            K_LOG_VERBOSE("Stream-rect-ignored-black row(%d) pos(%d, %d) srcRect(%d, %d)-(+%d, +%d).\n", i2, pos.x, pos.y, srcRect.x, srcRect.y, srcRect.width, srcRect.height);
                                        }
                                    }
                                }
                            }
                            //next row
                            rowsAddedCount++;
                        }
                        //next row
                        yTop += row->height;
                        if(yTop >= (layRect.y + layRect.height)){
                            //stop
                            break;
                        }
                    }
                    //
                    if(yTopBefore == yTop /*avoid-infinite-cycle*/ || yTop >= (layRect.y + layRect.height)){
                        //stop
                        break;
                    }
                }
            } //if(obj->layout.rows.use > 0 && obj->layout.rows.rectsCount > 0)
        } //if(fb != NULL)
    } //for(obj->fbs.use)
    return r;
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
        K_LOG_INFO("Thread, run-method started.\n");
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
        K_LOG_INFO("Thread, run-method ended.\n");
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
            K_LOG_INFO("Error calling 'pthread_attr_setdetachstate'.\n");
            exit(1);
        }
        {
            obj->stopFlag = 0;
            obj->isRunning = 1;
            if(0 != pthread_create(&obj->thread, &attrs, Thread_runMethod_, obj)){
                obj->isRunning = 0;
                K_LOG_INFO("Error calling 'pthread_create'.\n");
            } else {
                K_LOG_INFO("Thread, started.\n");
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
    K_LOG_VERBOSE("Touching: '%s' / %d / %d.\n", obj->device, obj->srcFmt, obj->dstFmt);
    gettimeofday(&obj->last, NULL);
    return 0;
}

//error codes

const STErrCode* _getErrCode(const int value){
    const STErrCode* r = NULL;
    int i; for(i = 0; i < sizeof(_errCodes) / sizeof(_errCodes[0]); i++){
        const STErrCode* err = &_errCodes[i];
        if(err->value == value){
            r = err;
            break;
        }
    }
    return r;
}

//Log

void __logInit(void){
    pthread_mutex_init(&_logMmutex, NULL);
}

void __logEnd(void){
    pthread_mutex_lock(&_logMmutex);
    {
        if(_logStream != NULL){
            fclose(_logStream);
            _logStream = NULL;
        }
        {
            if(_logBuffTmp != NULL){
                free(_logBuffTmp);
                _logBuffTmp = NULL;
            }
            _logBuffTmpSz = 0;
        }
    }
    pthread_mutex_unlock(&_logMmutex);
    pthread_mutex_destroy(&_logMmutex);
}

int __logOpenFile(const char* path){
    int r = -1;
    FILE* stream = fopen(path, "r+b");
    if(stream == NULL){
        //file doesnt exists, create new one
        stream = fopen(path, "w+b");
    }
    if(stream != NULL){
        K_LOG_INFO("Log file opened: '%s'.\n", path);
        if(-1 == fseek(stream, 0, SEEK_SET)){
            K_LOG_ERROR("Log could not seek to start-of-file.\n");
        } else {
            //read file untill first '0x03' (End-of-text ascci char) is found
            unsigned int iPos = 0, endOfTextFound = 0;
            char buff[1024 * 4];
            int read = fread(buff, 1, sizeof(buff), stream);
            while(read > 0 && !endOfTextFound){
                const char* c = buff;
                const char* cAfterEnd = c + read;
                while(c < cAfterEnd){
                    if(*c == 0x03){ //end-of-text ascci char
                        endOfTextFound = 1;
                        break;
                    }
                    c++; iPos++;
                }
                //read next
                read = fread(buff, 1, sizeof(buff), stream);
            }
            //
            if(endOfTextFound) iPos = 0;
            if(-1 == fseek(stream, iPos, SEEK_SET)){
                K_LOG_ERROR("Log could not seek writting start position: %u%s.\n", iPos, (endOfTextFound ? " (end-of-last-circular-jump)" : "end-of-file"));
            } else {
                //write end-of-text ascci char
                {
                    char v = 0x03;
                    fwrite(&v, sizeof(v), 1, stream);
                }
                fflush(stream);
                //return from end-of-text ascci char (to be overwritten on next call)
                if(-1 == fseek(stream, -1, SEEK_CUR)){
                    K_LOG_ERROR("Log could not seek -1 position.\n");
                } else {
                    const long pos = ftell(stream);
                    if(pos < 0){
                        K_LOG_ERROR("Log could not tell position.\n");
                    } else {
                        K_LOG_INFO("Log opened and starting at positon(%u%s): '%s'.\n", pos, (endOfTextFound ? " (end-of-last-circular-jump)" : "end-of-file"), path);
                        if(_logStream != NULL){
                            fclose(_logStream);
                            _logStream = NULL;
                        }
                        _logStream = stream; stream = NULL; //consume
                        _logStreamPos = pos;
                        r = 0;
                    }
                }
            }
        }
        //close (if not consumed)
        if(stream != NULL){
            fclose(stream);
            stream = NULL;
        }
    }
    return r;
}

unsigned int __logStrConcatUInt(const unsigned int value, const unsigned int digitsMin, char* dst, const unsigned int dstSz){
    unsigned int r = 0, m10 = 1, digits = 1, v = value, vd = 0;
    //Calculate digits
    while(v >= (m10 * 10) || digits < digitsMin){
        m10 *= 10; digits++;
    }
    //Add digits
    while((r + 2) < dstSz && digits > 0){
        vd            = (v / m10);    K_ASSERT_NATIVE(vd >= 0 && vd <= 9)
        v            -= (vd * m10);   K_ASSERT_NATIVE(v >= 0 && v <= value)
        dst[r++]    = ('0' + vd);
        m10 /= 10; digits--;
    }
    //
    return r;
}

void __log(const ENLogLevel lvl, const char* fmt, ...){
    if(lvl <= _logLvlMax){
        FILE* fout = NULL;
        {
            switch(lvl) {
                case ENLogLevel_Critical:
                case ENLogLevel_Error:
                case ENLogLevel_Warning:
                    if(!_logStdErrOff){
                        fout = stderr;
                    }
                    break;
                default:
                    if(!_logStdOutOff){
                        fout = stdout;
                    }
                    break;
            }
        }
        pthread_mutex_lock(&_logMmutex);
        if(fout != NULL || _logStream != NULL){
            va_list vargs;
            va_start(vargs, fmt);
            {
                char* fmt2 = (char*)fmt;
                int vargsApplied = 0; int vargsAppliedLen = 0;
                //Build new format
                {
                    const unsigned int fmtLen = strlen(fmt);
                    const char* prefix0 = NULL; unsigned int prefix0Sz = 0;
                    const char* preDate = NULL; unsigned int preDateSz = 0;
                    const char* prefix1 = NULL; unsigned int prefix1Sz = 0;
                    unsigned int fmt2Sz = 0; char dateStr[24]; //"YYYY-MM-DD hh:mm:ss.123\0"
                    memset(dateStr, 0, sizeof(dateStr));
                    //dateStr
                    {
                        int keepAdd = 1;
                        unsigned int i = 0; char* dst = dateStr;
                        time_t now = time(NULL);
                        struct tm* t_st = localtime(&now); //Note: 't_st' returned by 'localtime()' is a global variable.
                        //Year
                        if ((i + 4) < sizeof(dateStr) && keepAdd) { i += __logStrConcatUInt(t_st->tm_year + 1900, 4, &dst[i], (sizeof(dateStr) - i)); } else { keepAdd = 0; }
                        //Separator
                        if ((i + 1) < sizeof(dateStr) && keepAdd) { dst[i++] = '-'; } else { keepAdd = 0; }
                        //Month
                        if ((i + 2) < sizeof(dateStr) && keepAdd) { i += __logStrConcatUInt(t_st->tm_mon + 1, 2, &dst[i], (sizeof(dateStr) - i)); } else { keepAdd = 0; }
                        //Separator
                        if ((i + 1) < sizeof(dateStr) && keepAdd) { dst[i++] = '-'; } else { keepAdd = 0; }
                        //Day
                        if ((i + 2) < sizeof(dateStr) && keepAdd) { i += __logStrConcatUInt(t_st->tm_mday, 2, &dst[i], (sizeof(dateStr) - i)); } else { keepAdd = 0; }
                        //Separator
                        if ((i + 1) < sizeof(dateStr) && keepAdd) { dst[i++] = ' '; } else { keepAdd = 0; }
                        //Hour
                        if ((i + 2) < sizeof(dateStr) && keepAdd) { i += __logStrConcatUInt(t_st->tm_hour, 2, &dst[i], (sizeof(dateStr) - i)); } else { keepAdd = 0; }
                        //Separator
                        if ((i + 1) < sizeof(dateStr) && keepAdd) { dst[i++] = ':'; } else { keepAdd = 0; }
                        //Minute
                        if ((i + 2) < sizeof(dateStr) && keepAdd) { i += __logStrConcatUInt(t_st->tm_min, 2, &dst[i], (sizeof(dateStr) - i)); } else { keepAdd = 0; }
                        //Separator
                        if ((i + 1) < sizeof(dateStr) && keepAdd) { dst[i++] = ':'; } else { keepAdd = 0; }
                        //Secs
                        if ((i + 2) < sizeof(dateStr) && keepAdd) { i += __logStrConcatUInt(t_st->tm_sec, 2, &dst[i], (sizeof(dateStr) - i)); } else { keepAdd = 0; }
                        //Millisecs
                        /*if(t.ms > 0){
                            //Separator
                            if((i + 1) < sizeof(dateStr) && keepAdd){ dst[i++] = '.'; } else { keepAdd = 0; }
                            //Secs
                            if((i + 3) < sizeof(dateStr) && keepAdd){ i += NBDatetime_setDigitStr(t.ms, 3, &dst[i], (sizeof(dateStr) - i)); } else { keepAdd = 0; }
                        }*/
                        //End of string
                        if (sizeof(dateStr) > 0) {
                            dst[(i < sizeof(dateStr) ? i : (sizeof(dateStr) - 1))] = '\0';
                        }
                    }
                    //Set prefixes
                    {
                        prefix0 = "";
                        preDate = dateStr;
                        switch(lvl) {
                            case ENLogLevel_Critical: prefix1 = " CRITICAL, "; break;
                            case ENLogLevel_Error: prefix1 = " ERROR, "; break;
                            case ENLogLevel_Warning: prefix1 = " WARN, "; break;
                            default: prefix1 = " "; break;
                        }
                    }
                    //size
                    {
                        if(prefix0 != NULL) fmt2Sz += (prefix0Sz = strlen(prefix0));
                        if(preDate != NULL) fmt2Sz += (preDateSz = strlen(preDate));
                        if(prefix1 != NULL) fmt2Sz += (prefix1Sz = strlen(prefix1));
                        fmt2Sz += fmtLen;
                    }
                    //Build new fmt
                    if(fmt2Sz == fmtLen){
                        fmt2 = (char*)fmt;
                    } else {
                        unsigned int iPos = 0;
                        if(_logBuffTmpSz < (fmt2Sz + 1)){
                            if(_logBuffTmp != NULL){
                                free(_logBuffTmp);
                            }
                            _logBuffTmpSz    = (fmt2Sz + 1);
                            _logBuffTmp        = (char*)malloc(_logBuffTmpSz);
                        }
                        fmt2 = _logBuffTmp;
                        if(prefix0 != NULL){ memcpy(&fmt2[iPos], prefix0, prefix0Sz); iPos += prefix0Sz; }
                        if(preDate != NULL){ memcpy(&fmt2[iPos], preDate, preDateSz); iPos += preDateSz; }
                        if(prefix1 != NULL){ memcpy(&fmt2[iPos], prefix1, prefix1Sz); iPos += prefix1Sz; }
                        if(fmt != NULL){ memcpy(&fmt2[iPos], fmt, fmtLen); iPos += fmtLen; }
                        fmt2[iPos] = '\0';
                    }
                }
                //print
                if(fout != NULL){
                    if(vargsApplied){
                        fwrite(fmt2, vargsAppliedLen, 1, fout);
                    } else {
                        vfprintf(fout, fmt2, vargs);
                    }
                    fflush(fout);
                }
                //log file
                if(_logStream != NULL){
                    //write text
                    if(vargsApplied){
                        fwrite(fmt2, vargsAppliedLen, 1, _logStream);
                    } else {
                        vfprintf(_logStream, fmt2, vargs);
                    }
                    //write end-of-text ascci char
                    {
                        char v = 0x03;
                        fwrite(&v, sizeof(v), 1, _logStream);
                    }
                    fflush(_logStream);
                    //return from end-of-text ascci char (to be overwritten on next call)
                    if(fseek(_logStream, -1, SEEK_CUR) >= 0){
                        const long pos = ftell(_logStream);
                        if(pos >= 0){
                            _logStreamPos = pos; //ignoring the end-of-text ascci char
                            //circular file, return to start-of-file
                            if(_logStreamMaxSz > 0 && _logStreamPos >= _logStreamMaxSz){
                                if(fseek(_logStream, 0, SEEK_SET) >= 0){
                                    _logStreamPos = 0;
                                }
                            }
                        }
                    }
                }
            }
            va_end(vargs);
        }
        pthread_mutex_unlock(&_logMmutex);
    }
}

