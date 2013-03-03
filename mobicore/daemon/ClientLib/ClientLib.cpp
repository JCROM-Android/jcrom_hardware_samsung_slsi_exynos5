/** @addtogroup MCD_IMPL_LIB
 * @{
 * @file
 *
 * MobiCore Driver API.
 *
 * Functions for accessing MobiCore functionality from the normal world.
 * Handles sessions and notifications via MCI buffer.
 *
 * <!-- Copyright Giesecke & Devrient GmbH 2009 - 2012 -->
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior
 *    written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <stdint.h>
#include <stdbool.h>
#include <list>
#include "assert.h"

#include "public/MobiCoreDriverApi.h"

#include "mc_linux.h"
#include "Connection.h"
#include "CMutex.h"
#include "Device.h"
#include "mcVersionHelper.h"

#include "Daemon/public/MobiCoreDriverCmd.h"
#include "Daemon/public/mcVersion.h"

#include "log.h"

#include "Mci/mcimcp.h"

MC_CHECK_VERSION(DAEMON, 0, 2);

/** Notification data structure. */
typedef struct {
    uint32_t sessionId; /**< Session ID. */
    int32_t payload; /**< Additional notification information. */
} notification_t;

using namespace std;

list<Device *> devices;

// Forward declarations.
uint32_t getDaemonVersion(Connection *devCon, uint32_t *version);

CMutex devMutex;
//------------------------------------------------------------------------------
Device *resolveDeviceId(uint32_t deviceId)
{
    for (list<Device *>::iterator iterator = devices.begin();
            iterator != devices.end(); ++iterator) {
        Device  *device = (*iterator);

        if (device->deviceId == deviceId) {
            return device;
        }
    }
    return NULL;
}


//------------------------------------------------------------------------------
void addDevice(Device *device)
{
    devices.push_back(device);
}


//------------------------------------------------------------------------------
bool removeDevice(uint32_t deviceId)
{
    for (list<Device *>::iterator iterator = devices.begin();
            iterator != devices.end();
            ++iterator) {
        Device  *device = (*iterator);

        if (device->deviceId == deviceId) {
            devices.erase(iterator);
            delete device;
            return true;
        }
    }
    return false;
}

//------------------------------------------------------------------------------
// Parameter checking functions
// Note that android-ndk renames __func__ to __PRETTY_FUNCTION__
// see also /prebuilt/ndk/android-ndk-r4/platforms/android-8/arch-arm/usr/include/sys/cdefs.h

#define CHECK_DEVICE(device) \
    if (NULL == device) \
    { \
        mcResult = MC_DRV_ERR_UNKNOWN_DEVICE; \
        break; \
    }

#define CHECK_NOT_NULL(X) \
    if (NULL == X) \
    { \
        mcResult = MC_DRV_ERR_NULL_POINTER; \
        break; \
    }

#define CHECK_SESSION(S,SID) \
    if (NULL == S) \
    { \
        mcResult = MC_DRV_ERR_UNKNOWN_SESSION; \
        break; \
    }

//------------------------------------------------------------------------------
// Socket marshaling and checking functions
#define SEND_TO_DAEMON(CONNECTION, COMMAND, ...) \
{ \
    COMMAND ##_struct x = { \
        COMMAND, \
        __VA_ARGS__ \
    }; \
    int ret = CONNECTION->writeData(&x, sizeof x); \
    if(ret < 0) { \
        mcResult = MC_DRV_ERR_SOCKET_WRITE; \
        break; \
    } \
}

#define RECV_FROM_DAEMON(CONNECTION, RSP_STRUCT) \
{ \
    int rlen = CONNECTION->readData( \
            RSP_STRUCT, \
            sizeof(*RSP_STRUCT)); \
    if (rlen <= 0) { \
        mcResult = MC_DRV_ERR_SOCKET_READ; \
        break; \
    } \
    if (rlen != sizeof(*RSP_STRUCT) && rlen != sizeof(mcDrvResponseHeader_t)) {\
        mcResult = MC_DRV_ERR_SOCKET_LENGTH; \
        break; \
    } \
}

//------------------------------------------------------------------------------
__MC_CLIENT_LIB_API mcResult_t mcOpenDevice(uint32_t deviceId)
{
    mcResult_t mcResult = MC_DRV_OK;

    Connection *devCon = NULL;

    devMutex.lock();

    do {
        Device *device = resolveDeviceId(deviceId);
        if (device != NULL) {
            
            mcResult = MC_DRV_ERR_DEVICE_ALREADY_OPEN;
            break;
        }

        // Handle SIGPIPE inside write()
        //  If Daemon crashes and ClientLib writes to named socket,
        //  a sigpipe is send to ClientLib/TLC and kills it.
        signal(SIGPIPE, SIG_IGN);

        // Open new connection to device
        devCon = new Connection();
        if (!devCon->connect(SOCK_PATH)) {
            
            mcResult = MC_DRV_ERR_SOCKET_CONNECT;
            break;
        }

        // Runtime check of Daemon version.
        char *errmsg;
        uint32_t version = 0;
        mcResult = getDaemonVersion(devCon, &version);
        if(mcResult != MC_DRV_OK) {
            break;
        }
        if (!checkVersionOkDAEMON(version, &errmsg)) {
            
            mcResult = MC_DRV_ERR_DAEMON_VERSION;
            break;
        }
        

        // Forward device open to the daemon and read result
        SEND_TO_DAEMON(devCon, MC_DRV_CMD_OPEN_DEVICE, deviceId);

        RECV_FROM_DAEMON(devCon, &mcResult);

        if (mcResult != MC_DRV_OK) {
            
            break;
        }

        // there is no payload to read

        device = new Device(deviceId, devCon);
        mcResult = device->open("/dev/" MC_USER_DEVNODE);
        if (mcResult != MC_DRV_OK) {
            delete device;
            // devCon is freed in the Device destructor
            devCon = NULL;
            
            break;
        }

        addDevice(device);

    } while (false);

    devMutex.unlock();
    if (mcResult != MC_DRV_OK) {
        if (devCon != NULL)
            delete devCon;
        
    } else {
        
    }

    return mcResult;
}


//------------------------------------------------------------------------------
__MC_CLIENT_LIB_API mcResult_t mcCloseDevice(
    uint32_t deviceId
)
{
    mcResult_t mcResult = MC_DRV_OK;
    devMutex.lock();
    
    do {
        Device *device = resolveDeviceId(deviceId);
        CHECK_DEVICE(device);

        Connection *devCon = device->connection;

        // Return if not all sessions have been closed
        // TODO-2012-08-31-haenellu: improve check, if device connection is dead, this makes no more sense.
        if (device->hasSessions()) {
            
            mcResult = MC_DRV_ERR_SESSION_PENDING;
            break;
        }

        SEND_TO_DAEMON(devCon, MC_DRV_CMD_CLOSE_DEVICE);

        RECV_FROM_DAEMON(devCon, &mcResult);

        if (mcResult != MC_DRV_OK) {
            
            break;
        }

        removeDevice(deviceId);

    } while (false);

    devMutex.unlock();
    return mcResult;
}


//------------------------------------------------------------------------------
__MC_CLIENT_LIB_API mcResult_t mcOpenSession(
    mcSessionHandle_t  *session,
    const mcUuid_t     *uuid,
    uint8_t            *tci,
    uint32_t           len
)
{
    mcResult_t mcResult = MC_DRV_OK;

    devMutex.lock();
    

    do {
        CHECK_NOT_NULL(session);
        CHECK_NOT_NULL(uuid);
        CHECK_NOT_NULL(tci);

        if (len > MC_MAX_TCI_LEN) {
            
            mcResult = MC_DRV_ERR_TCI_TOO_BIG;
            break;
        }

        // Get the device associated with the given session
        Device *device = resolveDeviceId(session->deviceId);
        CHECK_DEVICE(device);

        Connection *devCon = device->connection;

        // Get the physical address of the given TCI
        CWsm_ptr pWsm = device->findContiguousWsm(tci);
        if (pWsm == NULL) {
            
            mcResult = MC_DRV_ERR_WSM_NOT_FOUND;
            break;
        }

        if (pWsm->len < len) {
            
            mcResult = MC_DRV_ERR_TCI_GREATER_THAN_WSM;
            break;
        }

        SEND_TO_DAEMON(devCon, MC_DRV_CMD_OPEN_SESSION,
                       session->deviceId,
                       *uuid,
                       (uint32_t)0,
                       (uint32_t)pWsm->handle,
                       len);

        // Read command response
        RECV_FROM_DAEMON(devCon, &mcResult);

        if (mcResult != MC_DRV_OK) {
            // TODO-2012-09-06-haenellu: Remove this code once tests can handle it

            if (MC_DRV_ERROR_MAJOR(mcResult) != MC_DRV_ERR_MCP_ERROR) {
                
            } else {
                uint32_t mcpResult = MC_DRV_ERROR_MCP(mcResult);
                

                // IMPROVEMENT-2012-09-03-haenellu: Remove this switch case and use MCP code in tests.
                switch (mcpResult) {
                case MC_MCP_RET_ERR_WRONG_PUBLIC_KEY:
                    mcResult = MC_DRV_ERR_WRONG_PUBLIC_KEY;
                    break;
                case MC_MCP_RET_ERR_CONTAINER_TYPE_MISMATCH:
                    mcResult = MC_DRV_ERR_CONTAINER_TYPE_MISMATCH;
                    break;
                case MC_MCP_RET_ERR_CONTAINER_LOCKED:
                    mcResult = MC_DRV_ERR_CONTAINER_LOCKED;
                    break;
                case MC_MCP_RET_ERR_SP_NO_CHILD:
                    mcResult = MC_DRV_ERR_SP_NO_CHILD;
                    break;
                case MC_MCP_RET_ERR_TL_NO_CHILD:
                    mcResult = MC_DRV_ERR_TL_NO_CHILD;
                    break;
                case MC_MCP_RET_ERR_UNWRAP_ROOT_FAILED:
                    mcResult = MC_DRV_ERR_UNWRAP_ROOT_FAILED;
                    break;
                case MC_MCP_RET_ERR_UNWRAP_SP_FAILED:
                    mcResult = MC_DRV_ERR_UNWRAP_SP_FAILED;
                    break;
                case MC_MCP_RET_ERR_UNWRAP_TRUSTLET_FAILED:
                    mcResult = MC_DRV_ERR_UNWRAP_TRUSTLET_FAILED;
                    break;
                default:
                    // TODO-2012-09-06-haenellu: Remove line and adapt codes in tests.
                    mcResult = MC_DRV_ERR_MCP_ERROR;
                    break;
                }
            }
            break; // loading of Trustlet failed, unlock mutex and return
        }

        // read payload
        mcDrvRspOpenSessionPayload_t rspOpenSessionPayload;
        RECV_FROM_DAEMON(devCon, &rspOpenSessionPayload);

        // Register session with handle
        session->sessionId = rspOpenSessionPayload.sessionId;

        

        // Set up second channel for notifications
        Connection *sessionConnection = new Connection();
        if (!sessionConnection->connect(SOCK_PATH)) {
            
            delete sessionConnection;
            // Here we know we couldn't connect to the Daemon.
            // Maybe we should use existing connection to close Trustlet.
            mcResult = MC_DRV_ERR_SOCKET_CONNECT;
            break;
        }

        do {
            SEND_TO_DAEMON(sessionConnection, MC_DRV_CMD_NQ_CONNECT,
                           session->deviceId,
                           session->sessionId,
                           rspOpenSessionPayload.deviceSessionId,
                           rspOpenSessionPayload.sessionMagic);

            RECV_FROM_DAEMON(sessionConnection, &mcResult);

            if (mcResult != MC_DRV_OK) {
                
                break;
            }

        } while (0);
        if (mcResult != MC_DRV_OK) {
            delete sessionConnection;
            // Here we know we couldn't communicate well with the Daemon.
            // Maybe we should use existing connection to close Trustlet.
            break; // unlock mutex and return
        }

        // there is no payload.

        // Session has been established, new session object must be created
        device->createNewSession(session->sessionId, sessionConnection);

        

    } while (false);

// TODO: enable as soon as there are more error codes
//    if (mcResult == MC_DRV_ERR_SOCKET_WRITE || mcResult == MC_DRV_ERR_SOCKET_READ) {
//        LOG_E("Connection is dead, removing device.");
//        removeDevice(session->deviceId);
//    }

    devMutex.unlock();

    return mcResult;
}


//------------------------------------------------------------------------------
__MC_CLIENT_LIB_API mcResult_t mcCloseSession(mcSessionHandle_t *session)
{
    mcResult_t mcResult = MC_DRV_OK;

    
    devMutex.lock();
    do {
        CHECK_NOT_NULL(session);
        

        Device *device = resolveDeviceId(session->deviceId);
        CHECK_DEVICE(device);

        Connection *devCon = device->connection;

        Session *nqSession = device->resolveSessionId(session->sessionId);

        CHECK_SESSION(nqSession, session->sessionId);

        SEND_TO_DAEMON(devCon, MC_DRV_CMD_CLOSE_SESSION, session->sessionId);

        RECV_FROM_DAEMON(devCon, &mcResult);

        if (mcResult != MC_DRV_OK) {
            
            // TODO-2012-08-03-haenellu: Remove once tests can handle it.
            mcResult = MC_DRV_ERR_UNKNOWN_DEVICE;
            break;
        }

        bool r = device->removeSession(session->sessionId);
        assert(r == true);

    } while (false);

    if (mcResult == MC_DRV_ERR_SOCKET_WRITE || mcResult == MC_DRV_ERR_SOCKET_READ) {
       
        removeDevice(session->deviceId);
    }

    devMutex.unlock();

    return mcResult;
}


//------------------------------------------------------------------------------
__MC_CLIENT_LIB_API mcResult_t mcNotify(
    mcSessionHandle_t   *session
)
{
    mcResult_t mcResult = MC_DRV_OK;
    devMutex.lock();
    

    do {
        CHECK_NOT_NULL(session);
        

        Device *device = resolveDeviceId(session->deviceId);
        CHECK_DEVICE(device);

        Connection *devCon = device->connection;

        Session *nqsession = device->resolveSessionId(session->sessionId);
        CHECK_SESSION(nqsession, session->sessionId);

        SEND_TO_DAEMON(devCon, MC_DRV_CMD_NOTIFY, session->sessionId);
        // Daemon will not return a response
    } while (false);

    if (mcResult == MC_DRV_ERR_SOCKET_WRITE) {
        
        removeDevice(session->deviceId);
    }

    devMutex.unlock();
    return mcResult;
}


//------------------------------------------------------------------------------
__MC_CLIENT_LIB_API mcResult_t mcWaitNotification(
    mcSessionHandle_t  *session,
    int32_t            timeout
)
{
    mcResult_t mcResult = MC_DRV_OK;

    devMutex.lock();
    

    do {
        CHECK_NOT_NULL(session);
        

        Device *device = resolveDeviceId(session->deviceId);
        CHECK_DEVICE(device);

        Session  *nqSession = device->resolveSessionId(session->sessionId);
        CHECK_SESSION(nqSession, session->sessionId);

        Connection *nqconnection = nqSession->notificationConnection;
        uint32_t count = 0;

        // Read notification queue till it's empty
        for (;;) {
            notification_t notification;
            ssize_t numRead = nqconnection->readData(
                                  &notification,
                                  sizeof(notification_t),
                                  timeout);
            //Exit on timeout in first run
            //Later runs have timeout set to 0. -2 means, there is no more data.
            if (count == 0 && numRead == -2 ) {
                
                mcResult = MC_DRV_ERR_TIMEOUT;
                break;
            }
            if (count == 0 && numRead == 0 ) {
                
                removeDevice(session->deviceId);
                mcResult = MC_DRV_ERR_NOTIFICATION;
                break;
            }
            // After first notification the queue will be drained, Thus we set
            // no timeout for the following reads
            timeout = 0;

            if (numRead != sizeof(notification_t)) {
                if (count == 0) {
                    //failure in first read, notify it
                    mcResult = MC_DRV_ERR_NOTIFICATION;
                    
                    break;
                } else {
                    // Read of the n-th notification failed/timeout. We don't tell the
                    // caller, as we got valid notifications before.
                    mcResult = MC_DRV_OK;
                    break;
                }
            }

            count++;
            

            if (notification.payload != 0) {
                // Session end point died -> store exit code
                nqSession->setErrorInfo(notification.payload);

                mcResult = MC_DRV_INFO_NOTIFICATION;
                break;
            }
        } // for(;;)

    } while (false);

    devMutex.unlock();
    return mcResult;
}


//------------------------------------------------------------------------------
__MC_CLIENT_LIB_API mcResult_t mcMallocWsm(
    uint32_t    deviceId,
    uint32_t    align,
    uint32_t    len,
    uint8_t     **wsm,
    uint32_t    wsmFlags)
{
    mcResult_t mcResult = MC_DRV_ERR_UNKNOWN;

    

    devMutex.lock();

    do {
        Device *device = resolveDeviceId(deviceId);
        CHECK_DEVICE(device);

        CHECK_NOT_NULL(wsm);

        CWsm_ptr pWsm;
        mcResult = device->allocateContiguousWsm(len, &pWsm);
        if (mcResult != MC_DRV_OK) {
            
            break;
        }

        *wsm = (uint8_t *)pWsm->virtAddr;
        mcResult = MC_DRV_OK;

    } while (false);

    devMutex.unlock();

    return mcResult;
}


//------------------------------------------------------------------------------
__MC_CLIENT_LIB_API mcResult_t mcFreeWsm(
    uint32_t    deviceId,
    uint8_t     *wsm
)
{
    mcResult_t mcResult = MC_DRV_ERR_UNKNOWN;
    Device *device;

    devMutex.lock();

    

    do {

        // Get the device associated wit the given session
        device = resolveDeviceId(deviceId);
        CHECK_DEVICE(device);

        // find WSM object
        CWsm_ptr pWsm = device->findContiguousWsm(wsm);
        if (pWsm == NULL) {
            
            mcResult = MC_DRV_ERR_WSM_NOT_FOUND;
            break;
        }

        // Free the given virtual address
        mcResult = device->freeContiguousWsm(pWsm);
        if (mcResult != MC_DRV_OK) {
            
            break;
        }
        mcResult = MC_DRV_OK;

    } while (false);

    devMutex.unlock();

    return mcResult;
}

//------------------------------------------------------------------------------
__MC_CLIENT_LIB_API mcResult_t mcMap(
    mcSessionHandle_t  *sessionHandle,
    void               *buf,
    uint32_t           bufLen,
    mcBulkMap_t        *mapInfo
)
{
    mcResult_t mcResult = MC_DRV_ERR_UNKNOWN;
    static CMutex mutex;

    

    devMutex.lock();

    do {
        CHECK_NOT_NULL(sessionHandle);
        CHECK_NOT_NULL(mapInfo);
        CHECK_NOT_NULL(buf);

        // Determine device the session belongs to
        Device *device = resolveDeviceId(sessionHandle->deviceId);
        CHECK_DEVICE(device);

        Connection *devCon = device->connection;

        // Get session
        Session *session = device->resolveSessionId(sessionHandle->sessionId);
        CHECK_SESSION(session, sessionHandle->sessionId);

        

        // Register mapped bulk buffer to Kernel Module and keep mapped bulk buffer in mind
        BulkBufferDescriptor *bulkBuf;
        mcResult = session->addBulkBuf(buf, bufLen, &bulkBuf);
        if (mcResult != MC_DRV_OK) {
            
            break;
        }

        SEND_TO_DAEMON(devCon, MC_DRV_CMD_MAP_BULK_BUF,
                       session->sessionId,
                       (uint32_t)bulkBuf->handle,
                       (uint32_t)0,
                       (uint32_t)(bulkBuf->virtAddr) & 0xFFF,
                       bulkBuf->len);

        // Read command response
        RECV_FROM_DAEMON(devCon, &mcResult);

        if (mcResult != MC_DRV_OK) {
            
            // TODO-2012-09-06-haenellu: Remove once tests can handle it.
            mcResult = MC_DRV_ERR_DAEMON_UNREACHABLE;

            // Unregister mapped bulk buffer from Kernel Module and remove mapped
            // bulk buffer from session maintenance
            if (session->removeBulkBuf(buf) != MC_DRV_OK) {
                // Removing of bulk buffer not possible
                
            }
            break;
        }

        mcDrvRspMapBulkMemPayload_t rspMapBulkMemPayload;
        RECV_FROM_DAEMON(devCon, &rspMapBulkMemPayload);

        // Set mapping info for internal structures
        bulkBuf->sVirtualAddr = (void *)rspMapBulkMemPayload.secureVirtualAdr;
        // Set mapping info for Trustlet
        mapInfo->sVirtualAddr = bulkBuf->sVirtualAddr;
        mapInfo->sVirtualLen = bufLen;
        mcResult = MC_DRV_OK;

    } while (false);

//    // TODO: enable as soon as there are more error codes
//    if (mcResult == MC_DRV_ERR_SOCKET_WRITE || mcResult == MC_DRV_ERR_SOCKET_READ) {
//        LOG_E("Connection is dead, removing device.");
//        removeDevice(sessionHandle->deviceId);
//    }

    devMutex.unlock();

    return mcResult;
}

//------------------------------------------------------------------------------
__MC_CLIENT_LIB_API mcResult_t mcUnmap(
    mcSessionHandle_t  *sessionHandle,
    void               *buf,
    mcBulkMap_t        *mapInfo
)
{
    mcResult_t mcResult = MC_DRV_ERR_UNKNOWN;
    static CMutex mutex;

    

    devMutex.lock();

    do {
        CHECK_NOT_NULL(sessionHandle);
        CHECK_NOT_NULL(mapInfo);
        CHECK_NOT_NULL(buf);

        // Determine device the session belongs to
        Device *device = resolveDeviceId(sessionHandle->deviceId);
        CHECK_DEVICE(device);

        Connection  *devCon = device->connection;

        // Get session
        Session *session = device->resolveSessionId(sessionHandle->sessionId);
        CHECK_SESSION(session, sessionHandle->sessionId);

        uint32_t handle = session->getBufHandle(mapInfo->sVirtualAddr);
        if (handle == 0) {
            
            mcResult = MC_DRV_ERR_BLK_BUFF_NOT_FOUND;
            break;
        }

        

        SEND_TO_DAEMON(devCon, MC_DRV_CMD_UNMAP_BULK_BUF,
                       session->sessionId,
                       handle,
                       (uint32_t)(mapInfo->sVirtualAddr),
                       mapInfo->sVirtualLen);

        RECV_FROM_DAEMON(devCon, &mcResult);

        if (mcResult != MC_DRV_OK) {
            
            // TODO-2012-09-06-haenellu: Remove once tests can handle it.
            mcResult = MC_DRV_ERR_DAEMON_UNREACHABLE;
            break;
        }

        // Unregister mapped bulk buffer from Kernel Module and remove mapped
        // bulk buffer from session maintenance
        mcResult = session->removeBulkBuf(buf);
        if (mcResult != MC_DRV_OK) {
            
            break;
        }

        mcResult = MC_DRV_OK;

    } while (false);

    if (mcResult == MC_DRV_ERR_SOCKET_WRITE || mcResult == MC_DRV_ERR_SOCKET_READ) {
        
        removeDevice(sessionHandle->deviceId);
    }

    devMutex.unlock();

    return mcResult;
}


//------------------------------------------------------------------------------
__MC_CLIENT_LIB_API mcResult_t mcGetSessionErrorCode(
    mcSessionHandle_t   *session,
    int32_t             *lastErr
)
{
    mcResult_t mcResult = MC_DRV_OK;

    devMutex.lock();
    

    do {
        CHECK_NOT_NULL(session);
        CHECK_NOT_NULL(lastErr);

        // Get device
        Device *device = resolveDeviceId(session->deviceId);
        CHECK_DEVICE(device);

        // Get session
        Session *nqsession = device->resolveSessionId(session->sessionId);
        CHECK_SESSION(nqsession, session->sessionId);

        // get session error code from session
        *lastErr = nqsession->getLastErr();

    } while (false);

    devMutex.unlock();
    return mcResult;
}

//------------------------------------------------------------------------------
__MC_CLIENT_LIB_API mcResult_t mcDriverCtrl(
    mcDriverCtrl_t  param,
    uint8_t         *data,
    uint32_t        len
)
{
    
    return MC_DRV_ERR_NOT_IMPLEMENTED;
}

//------------------------------------------------------------------------------
__MC_CLIENT_LIB_API mcResult_t mcGetMobiCoreVersion(
    uint32_t  deviceId,
    mcVersionInfo_t *versionInfo
)
{
    mcResult_t mcResult = MC_DRV_OK;

    devMutex.lock();
    

    do {
        Device *device = resolveDeviceId(deviceId);

        CHECK_DEVICE(device);
        CHECK_NOT_NULL(versionInfo);

        Connection *devCon = device->connection;

        SEND_TO_DAEMON(devCon, MC_DRV_CMD_GET_MOBICORE_VERSION);

        // Read GET MOBICORE VERSION response.

        RECV_FROM_DAEMON(devCon, &mcResult);

        if (mcResult != MC_DRV_OK) {
            
            // TODO-2012-09-06-haenellu: Remove once tests can handle it.
            mcResult = MC_DRV_ERR_DAEMON_UNREACHABLE;
            break;
        }

        // Read payload.
        mcVersionInfo_t versionInfo_socket;
        RECV_FROM_DAEMON(devCon, &versionInfo_socket);

        *versionInfo = versionInfo_socket;

    } while (0);

    devMutex.unlock();
    return mcResult;
}


//------------------------------------------------------------------------------
// Only called by mcOpenDevice()
// Must be taken with devMutex locked.
uint32_t getDaemonVersion(Connection *devCon, uint32_t *version)
{
    assert(version != NULL);
    mcResult_t mcResult = MC_DRV_OK;
    uint32_t v = 0;

    

    do {
        SEND_TO_DAEMON(devCon, MC_DRV_CMD_GET_VERSION);

        RECV_FROM_DAEMON(devCon, &mcResult);

        if (mcResult != MC_DRV_OK) {
            
            // version is still 0, we don't further analyze response here.
            break;
        }

        RECV_FROM_DAEMON(devCon, &v);

    } while (0);

    if (MC_DRV_OK == mcResult) {
        *version = v;
    }

    return mcResult;
}

/** @} */
