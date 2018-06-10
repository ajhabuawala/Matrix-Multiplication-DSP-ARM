/** ============================================================================
 *  @file   tskMessage.c
 *
 *  @path   
 *
 *  @desc   This is simple TSK based application that uses MSGQ. It receives
 *          and transmits messages from/to the GPP and runs the DSP
 *          application code (located in an external source file)
 *
 *  @ver    1.10
 */


/*  ----------------------------------- DSP/BIOS Headers            */
#include "helloDSPcfg.h"
#include <gbl.h>
#include <sys.h>
#include <sem.h>
#include <msgq.h>
#include <pool.h>

/*  ----------------------------------- DSP/BIOS LINK Headers       */
#include <dsplink.h>
#include <platform.h>
#include <failure.h>

/*  ----------------------------------- Sample Headers              */
#include <helloDSP_config.h>
#include <tskMessage.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif


/* FILEID is used by SET_FAILURE_REASON macro. */
#define FILEID  FID_APP_C

/* Place holder for the MSGQ name created on DSP */
Uint8 dspMsgQName[DSP_MAX_STRLEN];

/* Number of iterations message transfers to be done by the application. */
extern Uint16 numTransfers;

    
void matrix_multiply(Uint16 *mat1,Uint16 *mat2,Uint32 *C, Uint16 row, Uint16 col, Uint16 size);


/** ============================================================================
 *  @func   TSKMESSAGE_create
 *
 *  @desc   Create phase function for the TSKMESSAGE application. Initializes
 *          the TSKMESSAGE_TransferInfo structure with the information that will
 *          be used by the other phases of the application.
 *
 *  @modif  None.
 *  ============================================================================
 */
Int TSKMESSAGE_create(TSKMESSAGE_TransferInfo** infoPtr)
{
    Int status = SYS_OK;
    MSGQ_Attrs msgqAttrs = MSGQ_ATTRS;
    TSKMESSAGE_TransferInfo* info = NULL;
    MSGQ_LocateAttrs syncLocateAttrs;

    /* Allocate TSKMESSAGE_TransferInfo structure that will be initialized
     * and passed to other phases of the application */
    *infoPtr = MEM_calloc(DSPLINK_SEGID, sizeof(TSKMESSAGE_TransferInfo), DSPLINK_BUF_ALIGN);
    if (*infoPtr == NULL)
    {
        status = SYS_EALLOC;
        SET_FAILURE_REASON(status);
    }
    else
    {
        info = *infoPtr;
        info->numTransfers = numTransfers;
        info->localMsgq = MSGQ_INVALIDMSGQ;
        info->locatedMsgq = MSGQ_INVALIDMSGQ;
    }

    if (status == SYS_OK)
    {
        /* Set the semaphore to a known state. */
        SEM_new(&(info->notifySemObj), 0);

        /* Fill in the attributes for this message queue. */
        msgqAttrs.notifyHandle = &(info->notifySemObj);
        msgqAttrs.pend = (MSGQ_Pend) SEM_pendBinary;
        msgqAttrs.post = (MSGQ_Post) SEM_postBinary;

        SYS_sprintf((Char *)dspMsgQName, "%s%d", DSP_MSGQNAME, GBL_getProcId());

        /* Creating message queue */
        status = MSGQ_open((String)dspMsgQName, &info->localMsgq, &msgqAttrs);
        if (status != SYS_OK)
        {
            SET_FAILURE_REASON(status);
        }
        else
        {
            /* Set the message queue that will receive any async. errors. */
            MSGQ_setErrorHandler(info->localMsgq, SAMPLE_POOL_ID);

            /* Synchronous locate.                           */
            /* Wait for the initial startup message from GPP */
            status = SYS_ENOTFOUND;
            while ((status == SYS_ENOTFOUND) || (status == SYS_ENODEV))
            {
                syncLocateAttrs.timeout = SYS_FOREVER;
                status = MSGQ_locate(GPP_MSGQNAME, &info->locatedMsgq, &syncLocateAttrs);
                if ((status == SYS_ENOTFOUND) || (status == SYS_ENODEV))
                {
                    TSK_sleep(1000);
                }
                else if(status != SYS_OK)
                {
#if !defined (LOG_COMPONENT)
                    LOG_printf(&trace, "MSGQ_locate (msgqOut) failed. Status = 0x%x\n", status);
#endif
                }
            }
        }
       /* Initialize the sequenceNumber */
        info->sequenceNumber = 0;
    }

    return status;
}


/** ============================================================================
 *  @func   TSKMESSAGE_execute
 *
 *  @desc   Execute phase function for the TSKMESSAGE application. Application
 *          receives a message, verifies the id and executes the DSP processing.
 *
 *  @modif  None.
 *  ============================================================================
 */
Int TSKMESSAGE_execute(TSKMESSAGE_TransferInfo* info)
{
    Int status = SYS_OK;
    Uint32 i;
    ControlMsg *msg;
    Uint16 *mat1, *mat2;
    Uint16 ii;
    Uint16 jj;

    /* Allocate and send the message */
    status = MSGQ_alloc(SAMPLE_POOL_ID, (MSGQ_Msg*) &msg, APP_BUFFER_SIZE);

    if (status == SYS_OK)
    {
        MSGQ_setMsgId((MSGQ_Msg) msg, info->sequenceNumber);
        MSGQ_setSrcQueue((MSGQ_Msg) msg, info->localMsgq);
        msg->command = 0x01;
        //SYS_sprintf(msg->arg1, "DSP is awake!");
        msg->data.arg1[0]=111;
        status = MSGQ_put(info->locatedMsgq, (MSGQ_Msg) msg);
        if (status != SYS_OK)
        {
            /* Must free the message */
            MSGQ_free ((MSGQ_Msg) msg);
            SET_FAILURE_REASON(status);
        }
    }
    else
    {
        SET_FAILURE_REASON(status);
    }


    info->numTransfers = 3;
    /* Execute the loop for the configured number of transfers  */
    /* A value of 0 in numTransfers implies infinite iterations */
    for (i = 0; (((info->numTransfers == 0) || (i < info->numTransfers)) && (status == SYS_OK)); i++)
    {
        /* Receive a message from the GPP */
        status = MSGQ_get(info->localMsgq,(MSGQ_Msg*) &msg, SYS_FOREVER);
        if (status == SYS_OK)
        {
            /* Check if the message is an asynchronous error message */
            if (MSGQ_getMsgId((MSGQ_Msg) msg) == MSGQ_ASYNCERRORMSGID)
            {
#if !defined (LOG_COMPONENT)
                LOG_printf(&trace, "Transport error Type = %d",((MSGQ_AsyncErrorMsg *) msg)->errorType);
#endif
                /* Must free the message */
                MSGQ_free((MSGQ_Msg) msg);
                status = SYS_EBADIO;
                SET_FAILURE_REASON(status);
            }
            /* Check if the message received has the correct sequence number */
            else if (MSGQ_getMsgId ((MSGQ_Msg) msg) != info->sequenceNumber)
            {
#if !defined (LOG_COMPONENT)
                LOG_printf(&trace, "Out of sequence message!");
#endif
                MSGQ_free((MSGQ_Msg) msg);
                status = SYS_EBADIO;
                SET_FAILURE_REASON(status);
            }
            else
            {
		/* Include your control flag or processing code here */ 
                
                if(i==0)
                {
                
                   mat1=(Uint16 *)malloc((msg->size)*(msg->size)*sizeof(Uint16));
                   mat2=(Uint16 *)malloc((msg->size)*(msg->size)*sizeof(Uint16));

                //memcpy(mat1, &(msg->arg1[0]),msg-size*msg->size*sizeof(Uint16) );
                    for(ii=0;ii<msg->size;ii++)
                    {
                        for(jj=0;jj<msg->size;jj++)
                        {
                            mat1[jj+ii*msg->size]= msg->data.arg1[jj+ii*msg->size];                   
                        }

                    }
                
                }
                if(i==1)
                {
                //memcpy(mat2, &(msg->arg1[0]),msg->size*msg->size*sizeof(Uint16) );
                
                    for(ii=0;ii<msg->size;ii++)
                    {
                        for(jj=0;jj<msg->size;jj++)
                        {
                           mat2[jj+ii*msg->size]= msg->data.arg1[jj+ii*msg->size];
                        }
                       
                    }
                     
                    msg->command = 0x02;
                
                    matrix_multiply(mat1,mat2,&(msg->data.arg2[0]),msg->size/2,msg->size,msg->size);
                
                }
                
            
                if(i==2)
                {
                     
                    msg->command = 0x02;
                
                    matrix_multiply(mat1+(msg->size*msg->size)/2,mat2,&(msg->data.arg2[0]),msg->size/2,msg->size,msg->size);
                
                } 
                
                


                /* Increment the sequenceNumber for next received message */
                info->sequenceNumber++;
                /* Make sure that sequenceNumber stays within the range of iterations */
                if (info->sequenceNumber == MSGQ_INTERNALIDSSTART)
                {
                    info->sequenceNumber = 0;
                }
                MSGQ_setMsgId((MSGQ_Msg) msg, info->sequenceNumber);
                MSGQ_setSrcQueue((MSGQ_Msg) msg, info->localMsgq);

                
                /* Send the message back to the GPP*/
                
                status = MSGQ_put(info->locatedMsgq,(MSGQ_Msg) msg);
                if (status != SYS_OK)
                {
                    SET_FAILURE_REASON(status);
                }
                
            }
        }
        else
        {
            SET_FAILURE_REASON (status);
        }
    }
    

    return status;
}


/** ============================================================================
 *  @func   TSKMESSAGE_delete
 *
 *  @desc   Delete phase function for the TSKMESSAGE application. It deallocates
 *          all the resources of allocated during create phase of the
 *          application.
 *
 *  @modif  None.
 *  ============================================================================
 */
Int TSKMESSAGE_delete(TSKMESSAGE_TransferInfo* info)
{
    Int status = SYS_OK;
    Int tmpStatus = SYS_OK;
    Bool freeStatus = FALSE;

    /* Release the located message queue */
    if (info->locatedMsgq != MSGQ_INVALIDMSGQ)
    {
        status = MSGQ_release(info->locatedMsgq);
        if (status != SYS_OK)
        {
            SET_FAILURE_REASON(status);
        }
    }

     /* Reset the error handler before deleting the MSGQ that receives */
     /* the error messages.                                            */
    MSGQ_setErrorHandler(MSGQ_INVALIDMSGQ, POOL_INVALIDID);

    /* Delete the message queue */
    if (info->localMsgq != MSGQ_INVALIDMSGQ)
    {
        tmpStatus = MSGQ_close(info->localMsgq);
        if ((status == SYS_OK) && (tmpStatus != SYS_OK))
        {
            status = tmpStatus;
            SET_FAILURE_REASON(status);
        }
    }

    /* Free the info structure */
    freeStatus = MEM_free(DSPLINK_SEGID, info, sizeof(TSKMESSAGE_TransferInfo));
    if ((status == SYS_OK) && (freeStatus != TRUE))
    {
        status = SYS_EFREE;
        SET_FAILURE_REASON(status);
    }
    return status;
}


#if defined (__cplusplus)
}
#endif /* defined (__cplusplus) */



void matrix_multiply(Uint16 *mat1,Uint16 *mat2,Uint32 *C, Uint16 row, Uint16 col,Uint16 size)


{
    
    Uint16 ii,jj,kk;
    Uint32 sum1,sum2,sum3,sum4,sum5,sum6,sum7,sum8,sum9,sum10,sum11,sum12,sum13,sum14,sum15,sum16;
   
    #pragma MUST_ITERATE(,128,)
    for( ii = 0; ii < row; ii++)
    {
    #pragma MUST_ITERATE(,128,)
        for( jj = 0; jj < col; jj++)
        {

            *(C+jj+ii*col)=0;
            #pragma MUST_ITERATE(,8,)
            for(kk= 0; kk < size/16; kk++)
            {
               sum1= (*(mat1+kk*16+ii*size))   * (*(mat2+jj+(kk*16)*col));
               sum2= (*(mat1+kk*16+1+ii*size)) * (*(mat2+jj+(kk*16+1)*col));
               sum3= (*(mat1+kk*16+2+ii*size)) * (*(mat2+jj+(kk*16+2)*col));
               sum4= (*(mat1+kk*16+3+ii*size)) * (*(mat2+jj+(kk*16+3)*col));
               sum5= (*(mat1+kk*16+4+ii*size)) * (*(mat2+jj+(kk*16+4)*col));
               sum6= (*(mat1+kk*16+5+ii*size)) * (*(mat2+jj+(kk*16+5)*col));
               sum7= (*(mat1+kk*16+6+ii*size)) * (*(mat2+jj+(kk*16+6)*col));
               sum8= (*(mat1+kk*16+7+ii*size)) * (*(mat2+jj+(kk*16+7)*col));
               sum9= (*(mat1+kk*16+8+ii*size)) * (*(mat2+jj+(kk*16+8)*col));
               sum10= (*(mat1+kk*16+9+ii*size)) * (*(mat2+jj+(kk*16+9)*col));
               sum11= (*(mat1+kk*16+10+ii*size)) * (*(mat2+jj+(kk*16+10)*col));
               sum12= (*(mat1+kk*16+11+ii*size)) * (*(mat2+jj+(kk*16+11)*col));
               sum13= (*(mat1+kk*16+12+ii*size)) * (*(mat2+jj+(kk*16+12)*col));
               sum14= (*(mat1+kk*16+13+ii*size)) * (*(mat2+jj+(kk*16+13)*col));
               sum15= (*(mat1+kk*16+14+ii*size)) * (*(mat2+jj+(kk*16+14)*col));
               sum16= (*(mat1+kk*16+15+ii*size)) * (*(mat2+jj+(kk*16+15)*col));
               
               *(C+jj+ii*col)+=sum1+sum2+sum3+sum4+sum5+sum6+sum7+sum8+sum9+sum10+sum11+sum12+sum13+sum14+sum15+sum16;
            
            }
            
        }
    }
    
    
    
}




