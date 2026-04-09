/**
 ****************************************************************
 * @file iso14443b.c
 *
 * @brief  iso1443b protocol driver
 *
 * @author 
 *
 * 
 ****************************************************************
 */ 

/*
 * INCLUDE FILES
 ****************************************************************
 */	 

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "nfc.h"
#include "iso14443b.h" 


unsigned char  g_fwi = 4;//frame waiting time integer

//////////////////////////////////////////////////////////////////////
// 函数原型:    char pcdrequestb(unsigned char req_code, unsigned char AFI, unsigned char N, unsigned char *ATQB)
// 函数功能:    B型卡请求
// 入口参数:    req_code				// 请求代码	ISO14443_3B_REQIDL 0x00 -- 空闲的卡
//										//			ISO14443_3B_REQALL 0x08 -- 所有的卡
//				AFI						// 应用标识符，0x00：全选
//				N						// 时隙总数,取值范围0--4。
// 出口参数:    *ATQB					// 请求应答，11字节
// 返 回 值:    STATUS_SUCCESS -- 成功；其它值 -- 失败。
// 说    明:	-   
//////////////////////////////////////////////////////////////////////
unsigned char PcdRequestB(unsigned char req_code, unsigned char AFI, unsigned char N, unsigned char *ATQB)
{
		unsigned char  status = MI_ERR;

	  transceive_buffer  *pi;
	  pi = &mf_com_data;
	
	  mf_com_data.mf_command = PCD_TRANSCEIVE;
	  mf_com_data.mf_length = 3;
	  mf_com_data.mf_data[0] = ISO14443B_ANTICOLLISION;
		mf_com_data.mf_data[1] = AFI;
		mf_com_data.mf_data[2] = (req_code & 0x08) | (N&0x07);
    PcdSetTmo(5);
		status = pcd_com_transceive(pi);
	
		if (status !=  MI_OK && status != MI_NOTAGERR)
		{   
				status = MI_COLLERR;   
		}
		if (status == MI_OK && mf_com_data.mf_length != 96)
		{   
				status = MI_COM_ERR;   
		}
		if (status == MI_OK) 
		{	
				memcpy(ATQB, &mf_com_data.mf_data[0], 16);
        PcdSetTmo(ATQB[11]>>4); // set FWT 
        g_fwi = (ATQB[11]>>4);
    }
    return status;
}                      


//////////////////////////////////////////////////////////////////////
//SLOT-MARKER
//////////////////////////////////////////////////////////////////////
unsigned char PcdSlotMarker(unsigned char N, unsigned char *ATQB)
{
    unsigned char status;
	  transceive_buffer  *pi;
	  pi = &mf_com_data;
	
	  mf_com_data.mf_command = PCD_TRANSCEIVE;
	  mf_com_data.mf_length = 1;

		status = pcd_com_transceive(pi);

		PcdSetTmo(5);

    if(!N || N>15)
		{
			status = MI_WRONG_PARAMETER_VALUE;	
    }
		else
    {
			 mf_com_data.mf_data[0] = 0x05 |(N << 4);
			 status = pcd_com_transceive(pi);

			if (status != MI_OK && status != MI_NOTAGERR)
			{   
				status = MI_COLLERR;   
			}
			if (status == MI_OK && mf_com_data.mf_length != 96)
			{   
			status = MI_COM_ERR;   
			}
			if (status == MI_OK) 
			{	
				memcpy(ATQB, &mf_com_data.mf_data[0], 16);
				PcdSetTmo(ATQB[11]>>4); // set FWT 
				g_fwi = ATQB[11]>>4;
			} 	
    }
    return status;
}                      

            
//////////////////////////////////////////////////////////////////////
//ATTRIB
// 函数原型:    INchar PcdAttriB(unsigned char *PUPI, unsigned char pro_type, unsigned char CID, unsigned char *answer)
//
// 函数功能:    选择PICC
// 入口参数:    unsigned char *PUPI					// 4字节PICC标识符
//				unsigned char dsi_dri					// PCD<-->PICC 速率选择
//				unsigned char pro_type					// 支持的协议，由请求回应中的ProtocolType指定
// 返 回 值:    MI_OK -- 成功；其它值 -- 失败。
// 说    明:	-
//////////////////////////////////////////////////////////////////////
unsigned char PcdAttriB(unsigned char *PUPI, unsigned char dsi_dri, unsigned char pro_type, unsigned char CID, unsigned char *answer)
{
    unsigned char  status;
	
		transceive_buffer  *pi;
	  pi = &mf_com_data;
	  pro_type = pro_type;
	  mf_com_data.mf_command = PCD_TRANSCEIVE;
	  mf_com_data.mf_length = 9;
	  mf_com_data.mf_data[0] = ISO14443B_ATTRIB;
	  memcpy(&mf_com_data.mf_data[1], PUPI, 4);
		mf_com_data.mf_data[5] = 0x00;
		mf_com_data.mf_data[6] = ((dsi_dri << 4) | FSDI);
    mf_com_data.mf_data[7] = 0x01;
	  mf_com_data.mf_data[8] = (CID & 0x0f);
	
		PcdSetTmo(g_fwi);
    SetBitMask(0X1E, BIT7 | BIT6); //EOF SOF required

		status = pcd_com_transceive(pi);


    if (status == MI_OK)
    {	
    	*answer = mf_com_data.mf_data[0];
    } 	
    return status;
} 
//////////////////////////////////////////////////////////////////////
//获取B型卡ID
//////////////////////////////////////////////////////////////////////
unsigned char GetIdcardNum(unsigned char *pid)
{
    unsigned char  status;
		transceive_buffer  *pi;
	  pi = &mf_com_data;
    mf_com_data.mf_command = PCD_TRANSCEIVE;
	  mf_com_data.mf_length = 5;
	  mf_com_data.mf_data[0] = 0x00; //ISO14443B_ANTICOLLISION;     	       // APf code
		mf_com_data.mf_data[1] = 0x36; // AFI; 
		mf_com_data.mf_data[2] = 0x00; //((req_code<<3)&0x08) | (N&0x07);  // PARAM
		mf_com_data.mf_data[3] = 0x00;
		mf_com_data.mf_data[4] = 0x08;

		status = pcd_com_transceive(pi);
    if (status == MI_OK) 
    {	
    	memcpy(pid, &mf_com_data.mf_data[0], 10);
    } 
    return status;
}       

//////////////////////////////////////////////////////////////////////
// 函数原型:    char pcd_halt_b(unsigned char *PUPI)
// 函数功能:    挂起卡
// 入口参数:    INT8U *pPUPI					// 4字节PICC标识符
// 出口参数:    -
// 返 回 值:    MI_OK -- 成功；其它值 -- 失败。//////////////////////////////////////////////////////////////////////
unsigned char PcdHaltB(unsigned char *PUPI)
{
    unsigned char  status;
		transceive_buffer  *pi;
	  pi = &mf_com_data;
    mf_com_data.mf_command = PCD_TRANSCEIVE;
	  mf_com_data.mf_length = 5;
	  mf_com_data.mf_data[0] = ISO14443B_ATTRIB; //ISO14443B_ANTICOLLISION;     	       // APf code
	  memcpy(&	mf_com_data.mf_data[1], PUPI, 4);
    PcdSetTmo(g_fwi);		
	
		status = pcd_com_transceive(pi);
																							
    return status;
}   


/**
 ****************************************************************
 * @brief select_sr() 
 *
 * 防冲撞函数
 * @param: 
 * @param: 
 * @return: status 值为MI_OK:成功
 * @retval: chip_id  得到的SR卡片的chip_id
 ****************************************************************
 */
//unsigned char SelectSr(unsigned char *chip_id)
//{
//		unsigned char status;
//		unsigned int   unLen;
//		unsigned char  ucComMF522Buf[MAXRLEN];

//    PcdSetTmo(5);

//    ucComMF522Buf[0] = 0x06;     	       //initiate card
//    ucComMF522Buf[1] = 0;                
//    
//		status = PcdComMF522(PCD_TRANSCEIVE,ucComMF522Buf,2,ucComMF522Buf,&unLen);
// 
//    if (status!=MI_OK && status!=MI_NOTAGERR) 
//    {   
//				status = MI_COLLERR;       // collision occurs
//		}          
//    if(unLen != 8)
//    {   
//				status = MI_COM_ERR;  
//		}   
//    if (status == MI_OK)
//    {	
//        PcdSetTmo(5);

//        ucComMF522Buf[1] = ucComMF522Buf[0];     	       
//        ucComMF522Buf[0] = 0x0E;                 // Slect card
//          
//				status = PcdComMF522(PCD_TRANSCEIVE,ucComMF522Buf,2,ucComMF522Buf,&unLen);
//        if (status!=MI_OK && status!=MI_NOTAGERR)  // collision occurs
//        {   
//						status = MI_COLLERR; 						// collision occurs
//				}               
//        if (unLen != 8) 
//        {   
//						status = MI_COM_ERR;   
//				}
//        if (status == MI_OK)
//        { 
//						*chip_id = ucComMF522Buf[0]; 
//				}
//    } 	
//    return status;
//}  

////////////////////////////////////////////////////////////////////////
////SR176卡读块
////////////////////////////////////////////////////////////////////////
//unsigned char ReadSr176(unsigned char addr, unsigned char *readdata)
//{
//    unsigned char status;
//		unsigned int   unLen;
//		unsigned char  ucComMF522Buf[MAXRLEN];
//	
//    PcdSetTmo(5);
//	
//    ucComMF522Buf[0] = 0x08;
//    ucComMF522Buf[1] = addr;
//  
//    status = PcdComMF522(PCD_TRANSCEIVE,ucComMF522Buf,2,ucComMF522Buf,&unLen);

//    if ((status==MI_OK) && (unLen!=16))
//    {   
//				status = MI_BITCOUNTERR;   
//		}
//    if (status == MI_OK)
//    {
//        *readdata     = ucComMF522Buf[0];
//        *(readdata+1) = ucComMF522Buf[1];
//    }
//    return status;  
//}  
////////////////////////////////////////////////////////////////////////
////SR176卡写块
////////////////////////////////////////////////////////////////////////
//unsigned char WriteSr176(unsigned char addr, unsigned char *writedata)
//{
//    unsigned char status;
//		unsigned int   unLen;
//		unsigned char  ucComMF522Buf[MAXRLEN];

//    PcdSetTmo(5);
//	
//    ucComMF522Buf[0] = 9;
//    ucComMF522Buf[1] = addr;
//    ucComMF522Buf[2] = *writedata;
//    ucComMF522Buf[3] = *(writedata+1);

//		status = PcdComMF522(PCD_TRANSCEIVE,ucComMF522Buf,4,ucComMF522Buf,&unLen);

//    return status;  
//}      

//  	
////////////////////////////////////////////////////////////////////////
////SR176卡块锁定
////////////////////////////////////////////////////////////////////////
//unsigned char ProtectSr176(unsigned char lockreg)
//{
//    unsigned char status;
//		unsigned int   unLen;
//		unsigned char  ucComMF522Buf[MAXRLEN];

//    PcdSetTmo(5);

//		ucComMF522Buf[0] = 0x09;
//    ucComMF522Buf[1] = 0x0F;
//    ucComMF522Buf[2] = 0;
//    ucComMF522Buf[3] = lockreg;
//    
//		status = PcdComMF522(PCD_TRANSCEIVE,ucComMF522Buf,4,ucComMF522Buf,&unLen);

//    return status;  
//}   

////////////////////////////////////////////////////////////////////////
////COMPLETION ST
////////////////////////////////////////////////////////////////////////
//unsigned char CompletionSr()
//{
//    unsigned char status;
//		unsigned int   unLen;
//		unsigned char  ucComMF522Buf[MAXRLEN];

//    PcdSetTmo(5);
//    ucComMF522Buf[0] = 0x0F;
//	
//    status = PcdComMF522(PCD_TRANSCEIVE,ucComMF522Buf,1,ucComMF522Buf,&unLen);
//	
//    return status;  
//}                                          



