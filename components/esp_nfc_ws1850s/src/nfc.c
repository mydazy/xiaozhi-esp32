/**
***********************************************************************************************
* @file ncf.c
* @brief nfc函数实现.
* @author Ginger
* @date 2021.11.01
* @version v1.0
* @par Copyright (c):
*
* @par History:
*
**********************************************************************************************
*/
#include "nfc.h"
#include "ws1850_iic.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "iso14443b.h"

transceive_buffer mf_com_data;

int pcd_com_transceive(struct transceive_buffer *pi);

unsigned char uid_length = 0;																									  // uid长度
unsigned char CT[2];																											  // 卡类型
unsigned char IDA[10];																											  // 存A卡ID
unsigned char Block = 0x05;																										  // 演示读M1卡扇区1的第二块
unsigned char pps_pcmd = 0;																										  // pps参数
unsigned char PassWd[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};																	  // M1卡扇区默认秘钥
unsigned char PassWd_User[16] = {0x01, 0x01, 0x02, 0x02, 0x03, 0x03, 0xFF, 0x07, 0x80, 0x69, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; // 客户修改秘钥
unsigned char IDB[10];																											  // 存身份证ID
unsigned char PUPI[4];																											  // 存B卡ID
unsigned char RWDATA[16];
unsigned char Write_DATA[16] = {0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01};
unsigned char uid[10];
unsigned char RID[7] = {0x78, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}; // M0 card
/***  Mifare_UltraLight 调试参数 ******/
unsigned char AUTH0[4] = {0x04, 0, 0, 0x08};	 // 29h ,byte3是定义认证保护的起始页地址，比如写0x08，其需要校验秘钥区为（0x08-0x2c）
unsigned char Pwd[4] = {0xff, 0xff, 0xff, 0xff}; // 2Bh,秘钥存放地址, 四个字节
unsigned char pack[4] = {0xff, 0xff, 0x00, 0x00};
unsigned char auth_pack[2]; // 校验秘钥返回数据
unsigned char fast_readbuff[64];
unsigned char ws_access[4] = {0x80, 0x00, 0x00, 0x00}; // 2Ah的byte0, bit7控制经过秘钥才能操作user区, 0表示写需要经过秘钥验证, 1表示读写都需要经过秘钥验证
unsigned char ul_writedata[64] = {0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01};

void user_print_hex(const char *prefix, const unsigned char *data, int length)
{
	if (prefix != NULL)
	{
		printf("%s", prefix);
	}

	for (int i = 0; i < length; i++)
	{
		printf("%02X", data[i]);

		// 每个字节后加空格（最后一个不加）
		if (i < length - 1)
		{
			printf(" ");
		}

		// 每16字节换行并缩进
		if ((i + 1) % 16 == 0 && (i + 1) < length)
		{
			if (prefix != NULL)
			{
				printf("\n%*s", (int)strlen(prefix), ""); // 对齐前缀
			}
			else
			{
				printf("\n");
			}
		}
	}

	printf("\n");
}

/**
 * @brief us延时函数
 * @param n：延时变量
 * @return 无
 */
void delay_us(unsigned int n)
{
	esp_rom_delay_us(n);
}
/**
 * @brief ms延时函数
 * @param n：延时变量
 * @return 无
 */
void delay_ms(unsigned int n)
{
#if 0
  unsigned int j,k;
  while(n)
  {
    j=20;     //10~1ms
    do{
      k=31;
      do{
        ;;;//nop();nop();
      } while(--k);
    } while(--j);
    n--;	
  }
#else

	vTaskDelay(pdMS_TO_TICKS(n));
#endif
}
/////////////////////////////////////////////////////////////////////
// 功    能：复位芯片
// 返    回: 成功返回MI_OK
/////////////////////////////////////////////////////////////////////

void PcdReset(void)
{
#if 0 // hard reset  //
	CLR_NFC_RST;
	delay_ms(5);
	SET_NFC_RST;
	delay_ms(5); // 请客户确认delay函数延时准确, 晶振起振时间要小于此值，并且留有余量。
#endif
#if 1 /**** 软复位****/
	WriteRawRC(CommandReg,PCD_RESETPHASE);	
	delay_ms(3);
#endif
	/********复位读版本号，测通讯是否成功***************/
	// 	int temp;
	//  temp = ReadRawRC(0x37);
	//  printf("Version : 0x%02x\n", temp);
}

/////////////////////////////////////////////////////////////////////
// 设置PCD定时器
// input:fwi=0~15
/////////////////////////////////////////////////////////////////////
void PcdSetTmo(unsigned char fwi)

{
	WriteRawRC(TPrescalerReg, (TP_FWT_302us) & 0xFF);
	WriteRawRC(TModeReg, BIT7 | (((TP_FWT_302us) >> 8) & 0xFF));

	WriteRawRC(TReloadRegL, (1 << fwi) & 0xFF);
	WriteRawRC(TReloadRegH, ((1 << fwi) & 0xFF00) >> 8);
}
////////////////////////////////////////////////////////////

void pcd_delay_sfgi(unsigned char sfgi)
{
	// SFGT = (SFGT+dSFGT) = [(256 x 16/fc) x 2^SFGI] + [384/fc x 2^SFGI]
	// dSFGT =  384 x 2^FWI / fc
	WriteRawRC(TPrescalerReg, (TP_FWT_302us + TP_dFWT) & 0xFF);
	WriteRawRC(TModeReg, BIT7 | (((TP_FWT_302us + TP_dFWT) >> 8) & 0xFF));

	if (sfgi > 14 || sfgi < 1)
	{ // FDTA,PCD,MIN = 6078 * 1 / fc
		sfgi = 1;
	}

	WriteRawRC(TReloadRegL, (1 << sfgi) & 0xFF);
	WriteRawRC(TReloadRegH, ((1 << sfgi) >> 8) & 0xFF);

	WriteRawRC(ComIrqReg, 0x7F); // 清除中断
	WriteRawRC(ComIEnReg, BIT0);
	ClearBitMask(TModeReg, BIT7); // clear TAuto
	SetBitMask(ControlReg, BIT6); // set TStartNow

	// while(!INT_PIN);// wait new INT
	CheckIrq();
	SetBitMask(TModeReg, BIT7); // recover TAuto

	// PcdSetTmo(g_pcd_module_info.ui_fwi); //recover timeout set
}

void CheckIrq(void)
{
/*判断TX,RX中断标志位*/
#if 0
unsigned char waitFor = 0x30;	
unsigned char n;
unsigned int i;	
 i = 200;
  do 
  {
    n = ReadRawRC(ComIrqReg);  //?????????
    i--;
  }
	while ((i!=0) && !(n&0x01) && !(n&waitFor));
#endif
/*查询Status1Reg 的bit4 IRQ中断标志*/
#if 1
	unsigned char n;
	unsigned int i;
	i = 500;
	do
	{
		n = ReadRawRC(Status1Reg) & 0x10; //?????????
		i--;
	} while ((i != 0) && (!n));
#endif
/*查询Status1Reg 的IRQ中断标志*/
#if 0
while(!(ReadRawRC(Status1Reg)&0x10));
#endif
/*固定延时*/
#if 0
delay_ms(2);
#endif
}

/*
****检测芯片版本信息***
****S版本号为0x15，T版本为0x18
*/

unsigned char IC_Version(void)
{
	unsigned char VERSION;
	VERSION = ReadRawRC(VersionReg);
	return VERSION;
}

/**
 * @brief 配置芯片的A/B模式
 * @param type：配置类型
 * @return 无
 */
void PcdConfig(unsigned char type)
{
	PcdAntennaOff();
	delay_ms(1);
	/***********TX Output Power*********************/
	WriteRawRC(GsNReg, 0x88);	//  值越大输出功率越大
	WriteRawRC(CWGsPReg, 0x20); // 值越大输出功率越大

	if ('A' == type)
	{
		ClearBitMask(Status2Reg, BIT3);
		ClearBitMask(ComIEnReg, BIT7); // 高电平触发中断
		WriteRawRC(ModeReg, 0x3D);	   // 11 // CRC seed:6363
		WriteRawRC(RxSelReg, 0x88);	   // RxWait
		WriteRawRC(TxASKReg, 0x40);	   // 15  //typeA
		WriteRawRC(TxModeReg, 0x00);   // 12 //Tx Framing A
		WriteRawRC(RxModeReg, 0x00);   // 13 //Rx framing A
		WriteRawRC(AutoTestReg, 0);
		WriteRawRC(0x0C, 0x00); //^_^
								// 以下寄存器必须按顺序配置
		{
			unsigned char backup;
			backup = ReadRawRC(0x37);
			WriteRawRC(0x37, 0x00);

			if (backup == 0x15)
			{
				WriteRawRC(0x37, 0x5E);
				WriteRawRC(0x26, 0x48);
				WriteRawRC(0x17, 0x88);
				WriteRawRC(0x35, 0xED);
				WriteRawRC(0x3b, 0xA5);
				WriteRawRC(0x37, 0xAE);
				WriteRawRC(0x3b, 0x72);
			}
			/*兼容配置，T板芯片打开*/
			if (backup == 0x18)
			{
				WriteRawRC(0x1d, 0x04); //
				WriteRawRC(0x37, 0xA5);
				WriteRawRC(0x32, 0xC9);
				WriteRawRC(0x33, 0x24);
				WriteRawRC(0x37, 0xAE);
				WriteRawRC(0x33, 0x59);
				WriteRawRC(0x31, 0x08);
				WriteRawRC(0x37, 0x5E);
				WriteRawRC(0x35, 0xED);
				WriteRawRC(0x3a, 0x10);
			}
			WriteRawRC(0x37, backup);
		}
	}
	else if ('B' == type)
	{
		WriteRawRC(Status2Reg, 0x00);  // 清MFCrypto1On
		ClearBitMask(ComIEnReg, BIT7); // 高电平触发中断
		WriteRawRC(ModeReg, 0x3F);	   // CRC seed:FFFF
		WriteRawRC(RxSelReg, 0x88);	   // RxWait
		WriteRawRC(0x0C, 0x00);		   //^_^
		// Tx
		WriteRawRC(ModGsPReg, 0x12); // 调制指数，29h值越大，B卡调制深度越小，反之。
		WriteRawRC(AutoTestReg, 0x00);
		WriteRawRC(TxASKReg, 0x00); // typeB
		WriteRawRC(TypeBReg, 0x13);
		WriteRawRC(TxModeReg, 0x83);	 // Tx Framing B
		WriteRawRC(RxModeReg, 0x83);	 // Rx framing B
		WriteRawRC(BitFramingReg, 0x00); // TxLastBits=0
		{
			unsigned char backup;
			backup = ReadRawRC(0x37);
			WriteRawRC(0x37, 0x00);
			if (backup == 0x15)
			{
				WriteRawRC(0x37, 0x5E);
				WriteRawRC(0x26, 0x48);
				WriteRawRC(0x17, 0x88);
				WriteRawRC(0x35, 0xED);
				WriteRawRC(0x3b, 0xE5);
			}
			/*兼容配置，T板芯片打开*/
			if (backup == 0x18)
			{
				WriteRawRC(0x1d, 0x04); //
				WriteRawRC(0x37, 0xA5);
				WriteRawRC(0x32, 0xC9);
				WriteRawRC(0x33, 0x24);
				WriteRawRC(0x37, 0xAE);
				WriteRawRC(0x33, 0x59);
				WriteRawRC(0x31, 0x08);
				WriteRawRC(0x37, 0x5E);
				WriteRawRC(0x35, 0xED);
				WriteRawRC(0x3a, 0x10);
			}
			WriteRawRC(0x37, backup);
		}
	}
	/*开场和延时，延时不要低于2ms，太小手机nfc会刷不到*/
	PcdAntennaOn();
	delay_ms(3);
}



// /**
//  * @brief 配置芯片的A/B模式
//  * @param type：配置类型
//  * @return 无
//  */
// void PcdConfig(unsigned char type)
// {
// 	PcdAntennaOff();
// 	delay_ms(1);
// 	/***********TX Output Power*********************/
// 	// WriteRawRC(GsNReg,  0xFF);	//  值越大输出功率越大
// 	// WriteRawRC(CWGsPReg, 0x3F); // 值越大输出功率越大

// 	WriteRawRC(0x29, 0x3F);

// 	// WriteRawRC(ModGsPReg, 0x00);	//hsf TX: N-driver off during modulation → full 100% ASK depth

	

// 	if ('A' == type)
// 	{
// 		ClearBitMask(Status2Reg, BIT3);
// 		ClearBitMask(ComIEnReg, BIT7); // 高电平触发中断
// 		WriteRawRC(ModeReg, 0x3D);	   // 11 // CRC seed:6363
// 		WriteRawRC(RxSelReg, 0x88);	   // RxWait
// 		WriteRawRC(0x26, 0x7F);  // hsf 48  ->  7F   // 接受增益
// 		WriteRawRC(TxASKReg, 0x40);	   // 15  //typeA
// 		WriteRawRC(TxModeReg, 0x00);   // 12 //Tx Framing A
// 		WriteRawRC(RxModeReg, 0x00);   // 13 //Rx framing A
// 		WriteRawRC(AutoTestReg, 0);
// 		WriteRawRC(0x0C, 0x00); //^_^
// 								// 以下寄存器必须按顺序配置
// 		{
// 			unsigned char backup;
// 			backup = ReadRawRC(0x37);
// 			WriteRawRC(0x37, 0x00);

// 			if (backup == 0x15)
// 			{
// 				WriteRawRC(0x37, 0x5E);
// 				WriteRawRC(0x26, 0x3F);  // hsf新加
// 				WriteRawRC(0x17, 0x88);
// 				WriteRawRC(0x35, 0xED);
// 				WriteRawRC(0x3b, 0xA5);
// 				WriteRawRC(0x37, 0xAE);
// 				WriteRawRC(0x3b, 0x72);
// 			}
// 			/*兼容配置，T板芯片打开*/
// 			if (backup == 0x18)
// 			{
// 				WriteRawRC(0x1d, 0x04); //
// 				WriteRawRC(0x37, 0xA5);
// 				WriteRawRC(0x32, 0xC9);
// 				WriteRawRC(0x33, 0x24);
// 				WriteRawRC(0x37, 0xAE);
// 				WriteRawRC(0x33, 0x59);
// 				WriteRawRC(0x31, 0x08);
// 				WriteRawRC(0x37, 0x5E);
// 				WriteRawRC(0x35, 0xED);
// 				WriteRawRC(0x3a, 0x10);
// 			}
// 			WriteRawRC(0x37, backup);
// 		}
// 	}
// 	else if ('B' == type)
// 	{
// 		WriteRawRC(Status2Reg, 0x00);  // 清MFCrypto1On
// 		ClearBitMask(ComIEnReg, BIT7); // 高电平触发中断
// 		WriteRawRC(ModeReg, 0x3F);	   // CRC seed:FFFF
// 		WriteRawRC(RxSelReg, 0x88);	   // RxWait
// 		WriteRawRC(0x0C, 0x00);		   //^_^
// 		// Tx
// 		WriteRawRC(ModGsPReg, 0x12); // 调制指数，29h值越大，B卡调制深度越小，反之。
// 		WriteRawRC(AutoTestReg, 0x00);
// 		WriteRawRC(TxASKReg, 0x00); // typeB
// 		WriteRawRC(TypeBReg, 0x13);
// 		WriteRawRC(TxModeReg, 0x83);	 // Tx Framing B
// 		WriteRawRC(RxModeReg, 0x83);	 // Rx framing B
// 		WriteRawRC(BitFramingReg, 0x00); // TxLastBits=0
// 		{
// 			unsigned char backup;
// 			backup = ReadRawRC(0x37);
// 			WriteRawRC(0x37, 0x00);
// 			if (backup == 0x15)
// 			{
// 				WriteRawRC(0x37, 0x5E);
// 				WriteRawRC(0x26, 0x48);
// 				WriteRawRC(0x17, 0x88);
// 				WriteRawRC(0x35, 0xED);
// 				WriteRawRC(0x3b, 0xE5);
// 			}
// 			/*兼容配置，T板芯片打开*/
// 			if (backup == 0x18)
// 			{
// 				WriteRawRC(0x1d, 0x04); //
// 				WriteRawRC(0x37, 0xA5);
// 				WriteRawRC(0x32, 0xC9);
// 				WriteRawRC(0x33, 0x24);
// 				WriteRawRC(0x37, 0xAE);
// 				WriteRawRC(0x33, 0x59);
// 				WriteRawRC(0x31, 0x08);
// 				WriteRawRC(0x37, 0x5E);
// 				WriteRawRC(0x35, 0xED);
// 				WriteRawRC(0x3a, 0x10);
// 			}
// 			WriteRawRC(0x37, backup);
// 		}
// 	}
// 	/*开场和延时，延时不要低于2ms，太小手机nfc会刷不到*/
// 	PcdAntennaOn();
// 	delay_ms(200);
// }

/**
 * @brief 关闭天线
 * @param 无
 * @return 无
 */
void PcdAntennaOff(void)
{
	WriteRawRC(TxControlReg, ReadRawRC(TxControlReg) & (~0x03));
}

/**
 * @brief 开启天线，每次启动或关闭天险发射之间应至少有1ms的间隔
 * @param 无
 * @return 无
 */
void PcdAntennaOn()
{
	WriteRawRC(TxControlReg, ReadRawRC(TxControlReg) | 0x03); // Tx1RFEn=1 Tx2RFEn=1
}

/**
* @brief  寻卡
* @param
					req_code[IN]:寻卡方式
					0x52 = 寻感应区内所有符合14443A标准的卡
					0x26 = 寻未进入休眠状态的卡
					pTagType[OUT]：卡片类型代码
					0x4400 = Mifare_UltraLight
					0x0400 = Mifare_One(S50)
					0x0200 = Mifare_One(S70)
					0x0800 = Mifare_Pro(X)
					0x4403 = Mifare_DESFire
* @return 成功返回MI_OK
*/
int PcdRequest(unsigned char req_code, unsigned char *pTagType)
{
	int status, temp;
	transceive_buffer *pi;
	pi = &mf_com_data;

#if log_enable
	printf("raqa :\n");
#endif

	WriteRawRC(BitFramingReg, 0x07); // Tx last bytes = 7
	ClearBitMask(TxModeReg, BIT7);	 // 不使能发送crc
	ClearBitMask(RxModeReg, BIT7);	 // 不使能接收crc
	ClearBitMask(Status2Reg, BIT3);	 // 清零MF crypto1认证成功标记
	PcdSetTmo(4);
	mf_com_data.mf_command = PCD_TRANSCEIVE;
	mf_com_data.mf_length = 1;
	mf_com_data.mf_data[0] = req_code;
	temp = ReadRawRC(0x37);

	// printf("VersionReg = 0x%02X\n", ReadRawRC(VersionReg));
	if (0x12 < temp && temp < 0x30)
	{
		// hsf
		// WriteRawRC(TxASKReg, 0x40); // 15  //typeA
		// WriteRawRC(PICC_CWG, 0x30);
		// WriteRawRC(PICC_CWP, 0x06);

	}
	else
	{
		WriteRawRC(TxASKReg, 0x40); // 15  //typeA
		WriteRawRC(PICC_CWG, 0x30);
		WriteRawRC(PICC_CWP, 0x06);
	}
	status = pcd_com_transceive(pi);

	if (!status && mf_com_data.mf_length != 0x10)
	{
		status = MI_BITCOUNTERR;
	}
	*pTagType = mf_com_data.mf_data[0];
	*(pTagType + 1) = mf_com_data.mf_data[1];

	return status;
}

/**********************T1T JEWEL CARD 接口*************************************/
/****
void UpdateCrc_B(uint8_t bCh, uint16_t *pLpwCrc)
{
	bCh = (bCh^(uint8_t)((*pLpwCrc)&0x00FFU));
	bCh = (bCh ^ (bCh<<4U));
	*pLpwCrc = (*pLpwCrc >> 8U) ^ ((uint16_t)bCh << 8U) ^ ((uint16_t)bCh << 3U) ^ ((uint16_t)bCh>>4U);
}

void Crc_Jewel( uint8_t *pData,uint32_t dwLength,uint8_t *pCrc)
{
	uint8_t bChBlock = 0;
	uint16_t wCrc = 0xFFFF;
	do
	{
			bChBlock = *pData++;
			UpdateCrc_B(bChBlock, &wCrc);
	} while (0u != (--dwLength));
	wCrc = ~wCrc;
	pCrc[0] = (uint8_t) (wCrc & 0xFFU);
	pCrc[1] = (uint8_t) ( (wCrc>>8U) & 0xFFU);
}

uint8_t PcdJewelCommand(uint8_t *pdata,uint8_t len,uint8_t *resp,uint8_t *replen)
{

	uint8_t status;
	transceive_buffer  *pi;
	  pi = &mf_com_data;
	uint8_t i;
	uint8_t crc[2];

	  ClearBitMask(TxModeReg, BIT7); //不使能发送crc
	  ClearBitMask(RxModeReg, BIT7); //不使能接收crc

	for(i=0;i<len-1;i++)
	{
		if(i==0)
		{
		   WriteRawRC(BitFramingReg,0x07);
		}
		else
		{
		   WriteRawRC(BitFramingReg,0x00);
		}
		mf_com_data.mf_command = PCD_TRANSMIT;
		mf_com_data.mf_length  = 1;
		mf_com_data.mf_data[0] = *(pdata+i);
	//    PcdSetTmo(8);
		status = JewelTransceive(pi);
				printf("PcdJewelCommand  1\n");
				printf("%02x",status);
				printf("\n");
		if(status != MI_OK)
		return status;
	}
			printf("PcdJewelCommand  2\n");
			delay_ms(2);
		mf_com_data.mf_command = PCD_TRANSCEIVE;
		mf_com_data.mf_length  = 1;
		mf_com_data.mf_data[0] = *(pdata+len-1);
		PcdSetTmo(8);
		status = JewelTransceive(pi);
				printf("PcdJewelCommand  2\n");
				printf(" %02x",status);
			printf("\n");
	if(status == MI_OK)
	{		printf("ok\n");

		*replen = mf_com_data.mf_length /8;
		if (*replen != 0)
		{
			memcpy(resp, &mf_com_data.mf_data[0], *replen);
			Crc_Jewel(resp,*replen-2,crc);
			if(crc[0]!=resp[*replen-2] || crc[1]!=resp[*replen-1])
			status = MI_CRCERR;
		}
	}
				printf(" %02x",status);
			printf("\n");

	return status;

}


uint8_t JewelTransceive(struct transceive_buffer *pi)
{
	uint8_t   status = MI_ERR;
	uint8_t   waitFor,n,lastBits;
	uint16_t  i;
	uint8_t   temp,irqEn;

	irqEn   = 0x77;
	waitFor = 0x30;
	WriteRawRC(CommandReg,PCD_IDLE);
	SetBitMask(FIFOLevelReg,0x80);
		WriteRawRC(ComIrqReg, 0x7F);// 清中断0
		WriteRawRC(DivIrqReg, 0x7F);// 清中断1
	WriteRawRC(MfRxReg,0x10);
	user_print_hex("tx : ", pi->mf_data ,pi->mf_length);
	for (i=0; i<pi->mf_length; i++)
	WriteRawRC(FIFODataReg, pi->mf_data[i]);

	WriteRawRC(ComIEnReg,irqEn|0x80);
	WriteRawRC(CommandReg, pi->mf_command);
	SetBitMask(BitFramingReg,0x80);
	do
	{
		temp = ReadRawRC(ComIrqReg);      //wait for TxIRQ
	} while((temp&0x40)==0);

	if(pi->mf_command == PCD_TRANSMIT)
	{
		if(ReadRawRC(ErrorReg)==0)
			 status = MI_OK;
		else
			 status = -1;
	}

	if(pi->mf_command == PCD_TRANSCEIVE)
	{
		WriteRawRC(MfRxReg,0x00);
		printf("enter PCD_TRANSCEIVE \n");
		for(i =1000;i>0;i--)
		{
			n = ReadRawRC(ComIrqReg);      //04h
			if((n&irqEn&0x01)||(n&waitFor))break;        //timerirq idleirq rxirq
		}
		printf("%02x",n);
				printf("\n");
		if (n&waitFor)       //IdleIRq
		{
					printf("read err reg \n");
			temp = ReadRawRC(ErrorReg);
					  printf("%02x",temp);
					  printf("\n");
			if(temp&0x1B)      //06h
			{
				status = MI_ERR;
			}
			else
			{
				status = MI_OK;
				if (pi->mf_command == PCD_TRANSCEIVE)
				{
					n = ReadRawRC(FIFOLevelReg);         //0ah
									 printf("%02x",n);
							 printf("\n");
					lastBits = ReadRawRC(ControlReg) & 0x07;     //0ch
					if (lastBits)
						pi->mf_length = (n-1)*8 + lastBits;
					else
						pi->mf_length = n*8;
					if (n == 0)    n = 1;
					if (n > MAXRLEN) n = MAXRLEN;
					for (i=0; i<n; i++)
						pi->mf_data[i] = ReadRawRC(FIFODataReg);  //09h
				}
								user_print_hex("Rx : ",pi->mf_data,n);
								printf("\n");
			}
		}
		else if (n & irqEn & 0x01)    //TimerIRq
		{
			status = MI_NOTAGERR;
		}
		else if (!(ReadRawRC(ErrorReg)&0x1B))
		{
			temp = ReadRawRC(ErrorReg);
			status = MI_ACCESSTIMEOUT;
		}
	}
	return status;
}



uint8_t PcdRidA(uint8_t *pUid)
{
	uint8_t  status;
	uint8_t cmd_rid[9]={0x78,0,0,0,0,0,0};  //{0x78,0,0,0,0,0,0,0xd0,0x43};
	uint8_t resp[8];
	uint8_t len;

	Crc_Jewel(cmd_rid,7,&cmd_rid[7]);
	status = PcdJewelCommand(cmd_rid,sizeof(cmd_rid),resp,&len);
		user_print_hex("RID : ",resp,8);
	if(status==MI_OK)
	{
		memcpy(pUid, &resp[2], 4);
	}
	return status;
}

uint8_t PcdReadA(uint8_t*pUid)
{
	uint8_t   status;
	uint8_t cmdbuf[9]={1,8,0};
	uint8_t  resp[8];
	uint8_t len;
	memcpy(&cmdbuf[3],pUid,4);
	Crc_Jewel(cmdbuf,7,&cmdbuf[7]);
	status = PcdJewelCommand(cmdbuf,sizeof(cmdbuf),resp,&len);
		user_print_hex("Read : ",resp,len);
	return status;
}

uint8_t PcdJewel(void)
{
	uint8_t   status;
	uint8_t uid[4];
	status = PcdRidA(uid);
	if(status!=MI_OK)
		return status;
	status = PcdReadA(uid);
   return status;
}
********/

/**
 ****************************************************************
 * @brief pcd_anticoll()
 *
 * 防冲撞函数
 * @param: select_code    0x93  cascaded level 1
 *                        0x95  cascaded level 2
 *                        0x97  cascaded level 3
 * @param: psnr 存放序列号(4byte)的内存单元首地址
 * @return: status 值为MI_OK:成功
 * @retval: psnr  得到的序列号放入指定单元
 ****************************************************************
 */
int pcd_cascaded_anticoll(unsigned char select_code, unsigned char coll_position, unsigned char *psnr)
{
	int status;
	unsigned char i;
	unsigned char temp;
	unsigned char bits;
	unsigned char bytes;
	// unsigned char coll_position;
	unsigned char snr_check;
	unsigned char snr[5];
	int sid_check;

	transceive_buffer *pi;
	pi = &mf_com_data;

#if log_enable
	printf("anticoll :\n");
#endif

	snr_check = 0;
	// coll_position = 0;
	memset(snr, 0, sizeof(snr));
	WriteRawRC(BitFramingReg, 0x00); // // Tx last bits = 0, rx align = 0
	ClearBitMask(TxModeReg, BIT7);	 // 不使能发送crc
	ClearBitMask(RxModeReg, BIT7);	 // 不使能接收crc
	sid_check = ReadRawRC(0x37);
	if (0x12 < sid_check && sid_check < 0x30)
		;
	else
	{
		WriteRawRC(TxModeReg, 0x30);
	}
	PcdSetTmo(4);
	do
	{
		bits = coll_position % 8;
		if (bits != 0)
		{
			bytes = coll_position / 8 + 1;
			ClearBitMask(BitFramingReg, TxLastBits | RxAlign);
			SetBitMask(BitFramingReg, (TxLastBits & (bits)) | (RxAlign & (bits << 4))); // tx lastbits , rx align
		}
		else
		{
			bytes = coll_position / 8;
		}
		mf_com_data.mf_command = PCD_TRANSCEIVE;
		mf_com_data.mf_data[0] = select_code;
		mf_com_data.mf_data[1] = 0x20 + ((coll_position / 8) << 4) + (bits & 0x0F);

		for (i = 0; i < bytes; i++)
		{
			mf_com_data.mf_data[i + 2] = snr[i];
		}
		mf_com_data.mf_length = bytes + 2;

		status = pcd_com_transceive(pi);

		temp = snr[coll_position / 8];
		if (status == MI_COLLERR)
		{
			for (i = 0; (5 >= coll_position / 8) && (i < 5 - (coll_position / 8)); i++)
			{
				snr[i + (coll_position / 8)] = mf_com_data.mf_data[i + 1];
			}
			snr[(coll_position / 8)] |= temp;
			if (mf_com_data.mf_data[0] >= bits)
			{
				coll_position += mf_com_data.mf_data[0] - bits;
			}
			else
			{
#if (0)
				printf("Err:coll_p  mf_data[0]=%02x < bits=%02x\n", (u16)mf_com_data.mf_data[0], (u16)bits);
#endif
			}
			// 保留冲突位以前的有效位
			snr[(coll_position / 8)] &= (0xff >> (8 - (coll_position % 8)));
			// 选择冲突bit位为1或是0的卡
			snr[(coll_position / 8)] |= 1 << (coll_position % 8); // 选择bit=1的卡
			// snr[(coll_position / 8)] &=  ~(1 << (coll_position % 8));//选择bit=0的卡
			coll_position++; // 冲突bit位增1
		}
		else if (status == MI_OK)
		{
			for (i = 0; i < (mf_com_data.mf_length / 8) && (i <= 4); i++) // 增加(i <= 4)防止snr[4-i]溢出
			{
				snr[4 - i] = mf_com_data.mf_data[mf_com_data.mf_length / 8 - i - 1];
			}
			snr[(coll_position / 8)] |= temp;
		}

	} while (status == MI_COLLERR);

	if (status == MI_OK)
	{
		for (i = 0; i < 4; i++)
		{
			*(psnr + i) = snr[i];
			snr_check ^= snr[i];
		}
		if (snr_check != snr[i])
		{
			status = MI_COM_ERR;
		}
	}

	WriteRawRC(BitFramingReg, 0x00); // // Tx last bits = 0, rx align = 0

	return status;
}

/**
 ****************************************************************
 * @brief pcd_select()
 *
 * 选定一张卡
 * @param: select_code    0x93  cascaded level 1
 *                        0x95  cascaded level 2
 *                        0x97  cascaded level 3
 * @param: psnr 存放序列号(4byte)的内存单元首地址
 * @return: status 值为MI_OK:成功
 * @retval: psnr  得到的序列号放入指定单元
 * @retval: psak  得到的Select acknolege 回复
 *
 *			  sak:
 *            Corresponding to the specification in ISO 14443, this function
 *            is able to handle extended serial numbers. Therefore more than
 *            one select_code is possible.
 *
 *            Select codes:
 *
 *            +----+----+----+----+----+----+----+----+
 *            | b8 | b7 | b6 | b5 | b4 | b3 | b2 | b1 |
 *            +-|--+-|--+-|--+-|--+----+----+----+-|--+
 *              |    |    |    |  |              | |
 *                                |              |
 *              1    0    0    1  | 001..std     | 1..bit frame anticoll
 *                                | 010..double  |
 *                                | 011..triple  |
 *
 *            SAK:
 *
 *            +----+----+----+----+----+----+----+----+
 *            | b8 | b7 | b6 | b5 | b4 | b3 | b2 | b1 |
 *            +-|--+-|--+-|--+-|--+-|--+-|--+-|--+-|--+
 *              |    |    |    |    |    |    |    |
 *                        |              |
 *                RFU     |      RFU     |      RFU
 *
 *                        1              0 .. UID complete, ATS available
 *                        0              0 .. UID complete, ATS not available
 *                        X              1 .. UID not complete
 *
 ****************************************************************
 */
int pcd_cascaded_select(unsigned char select_code, unsigned char *psnr, unsigned char *psak)
{
	unsigned char i;
	int status;
	unsigned char snr_check;
	transceive_buffer *pi;
	pi = &mf_com_data;
	snr_check = 0;
#if log_enable
	printf("select :\n");
#endif

	WriteRawRC(BitFramingReg, 0x00); // // Tx last bits = 0, rx align = 0
	SetBitMask(TxModeReg, BIT7);	 // 使能发送crc
	SetBitMask(RxModeReg, BIT7);	 // 使能接收crc
	PcdSetTmo(4);
	mf_com_data.mf_command = PCD_TRANSCEIVE;
	mf_com_data.mf_length = 7;
	mf_com_data.mf_data[0] = select_code;
	mf_com_data.mf_data[1] = 0x70;
	for (i = 0; i < 4; i++)
	{
		snr_check ^= *(psnr + i);
		mf_com_data.mf_data[i + 2] = *(psnr + i);
	}
	mf_com_data.mf_data[6] = snr_check;
	status = pcd_com_transceive(pi);
	if (status == MI_OK)
	{
		if (mf_com_data.mf_length != 0x8)
		{
			status = MI_BITCOUNTERR;
		}
		else
		{
			*psak = mf_com_data.mf_data[0];
		}
	}
	return status;
}

/**
 * @brief  用NFC计算CRC16函数
 * @param
 * @return
 */
void CalulateCRC(unsigned char *pIndata, unsigned char len, unsigned char *pOutData)
{
	unsigned char i, n;
	ClearBitMask(DivIrqReg, 0x04);
	WriteRawRC(CommandReg, PCD_IDLE);
	SetBitMask(FIFOLevelReg, 0x80);
	for (i = 0; i < len; i++)
	{
		WriteRawRC(FIFODataReg, *(pIndata + i));
	}
	WriteRawRC(CommandReg, PCD_CALCCRC);
	i = 0xFF;
	do
	{
		n = ReadRawRC(DivIrqReg);
		i--;
	} while ((i != 0) && !(n & 0x04));
	pOutData[0] = ReadRawRC(CRCResultRegL);
	pOutData[1] = ReadRawRC(CRCResultRegM);
}

/**
 ****************************************************************
 * @brief pcd_com_transceive()
 *
 * 通过芯片和ISO14443卡通讯
 *
 * @param: pi->mf_command = 芯片命令字
 * @param: pi->mf_length  = 发送的数据长度
 * @param: pi->mf_data[]  = 发送数据
 * @return: status 值为MI_OK:成功
 * @retval: pi->mf_length  = 接收的数据BIT长度
 * @retval: pi->mf_data[]  = 接收数据
 ****************************************************************
 */
int pcd_com_transceive(struct transceive_buffer *pi)
{

	unsigned char recebyte;
	int status = MI_OK;
	unsigned char irqEn;
	unsigned char waitFor;
	unsigned char lastBits;
	unsigned char i;
	unsigned char val;
	unsigned char err;

	unsigned char irq_inv;
	uint16_t len_rest;
	unsigned char len;
	//	unsigned char WATER_LEVEL;

	len = 0;
	len_rest = 0;
	err = 0;
	recebyte = 0;
	irqEn = 0;
	waitFor = 0;
	//	unsigned char i;

	switch (pi->mf_command)
	{
	case PCD_IDLE:
		irqEn = 0x00;
		waitFor = 0x00;
		break;
	case PCD_AUTHENT:
		irqEn = IdleIEn | TimerIEn;
		waitFor = IdleIRq;
		break;
	case PCD_RECEIVE:
		irqEn = RxIEn | IdleIEn;
		waitFor = RxIRq;
		recebyte = 1;
		break;
	case PCD_TRANSMIT:
		irqEn = TxIEn | IdleIEn;
		waitFor = TxIRq;
		break;
	case PCD_TRANSCEIVE:
		irqEn = RxIEn | IdleIEn | TimerIEn | TxIEn;
		waitFor = RxIRq;
		recebyte = 1;
		break;
	default:
		pi->mf_command = MI_UNKNOWN_COMMAND;
		break;
	}

	if (pi->mf_command != MI_UNKNOWN_COMMAND && (((pi->mf_command == PCD_TRANSCEIVE || pi->mf_command == PCD_TRANSMIT) && pi->mf_length > 0) || (pi->mf_command != PCD_TRANSCEIVE && pi->mf_command != PCD_TRANSMIT)))
	{
		WriteRawRC(CommandReg, PCD_IDLE);

		irq_inv = ReadRawRC(ComIEnReg) & BIT7;
		WriteRawRC(ComIEnReg, irq_inv | irqEn | BIT0); // 使能Timer 定时器中断
		WriteRawRC(ComIrqReg, 0x7F);				   // Clear INT
		WriteRawRC(DivIrqReg, 0x7F);				   // Clear INT
		// Flush Fifo
		SetBitMask(FIFOLevelReg, BIT7);
		if (pi->mf_command == PCD_TRANSCEIVE || pi->mf_command == PCD_TRANSMIT || pi->mf_command == PCD_AUTHENT)
		{
			len_rest = pi->mf_length;
			if (len_rest >= FIFO_SIZE)
			{
				len = FIFO_SIZE;
			}
			else
			{
				len = len_rest;
			}
#if log_enable
			user_print_hex("tx : ", pi->mf_data, pi->mf_length);
#endif
			for (i = 0; i < len; i++)
			{
				WriteRawRC(FIFODataReg, pi->mf_data[i]);
			}
			len_rest -= len; // Rest bytes
			if (len_rest != 0)
			{
				WriteRawRC(ComIrqReg, BIT2); // clear LoAlertIRq
				SetBitMask(ComIEnReg, BIT2); // enable LoAlertIRq
			}

			WriteRawRC(CommandReg, pi->mf_command);
			if (pi->mf_command == PCD_TRANSCEIVE || pi->mf_command == PCD_AUTHENT)  // hsf
			{
				SetBitMask(BitFramingReg, 0x80);
			}
			while (len_rest != 0)
			{
				CheckIrq();
				if (len_rest > (FIFO_SIZE - WATER_LEVEL))
				{
					len = FIFO_SIZE - WATER_LEVEL;
				}
				else
				{
					len = len_rest;
				}
				for (i = 0; i < len; i++)
				{
					WriteRawRC(FIFODataReg, pi->mf_data[pi->mf_length - len_rest + i]);
				}
				WriteRawRC(ComIrqReg, BIT2); // 在write fifo之后，再清除中断标记才可以
				len_rest -= len;			 // Rest bytes
				if (len_rest == 0)
				{
					ClearBitMask(ComIEnReg, BIT2); // disable LoAlertIRq
				}
			}
			// Wait TxIRq
			CheckIrq();
			val = ReadRawRC(ComIrqReg);
			if (val & TxIRq)
			{
				WriteRawRC(ComIrqReg, TxIRq);
				WriteRawRC(RxThresholdReg, CollLevel);
				WriteRawRC(RFCfgReg, GainLevel);
			}
		}
		if (PCD_RECEIVE == pi->mf_command)
		{
			WriteRawRC(CommandReg, PCD_RECEIVE);
			ClearBitMask(TModeReg, BIT7);
			SetBitMask(ControlReg, BIT6); // TStartNow
		}
		len_rest = 0;				 // bytes received
		WriteRawRC(ComIrqReg, BIT3); // clear HoAlertIRq
		SetBitMask(ComIEnReg, BIT3); // enable HoAlertIRq
		// 🔴 P0 修（2026-05）：原 while(1) 死循环无超时退出。
		//   触发：4G RF 干扰下 ComIrqReg BIT3/BIT5 反复翻转 → CheckIrq i=500 空读 →
		//        外层 while(1) 持续翻转 → NFC 任务僵死 → 用户必须重启。
		//   修：硬超时 ~50ms（200 轮 × 250µs，远超 ISO14443A 最长帧 ~10ms）。
		//        超时后用 transceive_timeout 标志压制后续 val 解析，正常走 line 1318 清理路径。
		uint32_t transceive_loop_guard = 200;
		bool transceive_timeout = false;
		while (1)
		{
			if (transceive_loop_guard-- == 0) {
				ClearBitMask(ComIEnReg, BIT3); // disable HoAlertIRq
				transceive_timeout = true;
				break;
			}
			CheckIrq();
			val = ReadRawRC(ComIrqReg);
			if ((val & BIT3) && !(val & BIT5))
			{
				if (len_rest + FIFO_SIZE - WATER_LEVEL > 255)
				{
					break;
				}
				for (i = 0; i < FIFO_SIZE - WATER_LEVEL; i++)
				{
					pi->mf_data[len_rest + i] = ReadRawRC(FIFODataReg);
				}
				WriteRawRC(ComIrqReg, BIT3); // 在read fifo之后，再清除中断标记才可以
				len_rest += FIFO_SIZE - WATER_LEVEL;
			}
			else
			{
				ClearBitMask(ComIEnReg, BIT3); // disable HoAlertIRq
				break;
			}
		}
		val = ReadRawRC(ComIrqReg);

		WriteRawRC(ComIrqReg, val); // 清中断

		if (val & BIT0)
		{ // 发生超时
			status = MI_NOTAGERR;
		}
		else
		{
			err = ReadRawRC(ErrorReg);

			status = MI_COM_ERR;
			if ((val & waitFor) && (val & irqEn))
			{
				if (!(val & ErrIRq))
				{ // 指令执行正确
					status = MI_OK;

					if (recebyte)
					{
						val = 0x7F & ReadRawRC(FIFOLevelReg);
						lastBits = ReadRawRC(ControlReg) & 0x07;
						if (len_rest + val > MAX_TRX_BUF_SIZE)
						{ // 长度过长超出缓存
							status = MI_COM_ERR;
						}
						else
						{
							if (lastBits && val) // 防止spi读错后 val-1成为负值
							{
								pi->mf_length = (val - 1) * 8 + lastBits;
							}
							else
							{
								pi->mf_length = val * 8;
							}
							pi->mf_length += len_rest * 8;

							if (val == 0)
							{
								val = 1;
							}
							for (i = 0; i < val; i++)
							{
								pi->mf_data[len_rest + i] = ReadRawRC(FIFODataReg);
							}
#if PICC_RETUERN_DATA
							user_print_hex("rx : ", pi->mf_data, pi->mf_length / 8 + !!(pi->mf_length % 8)); // pi->mf_length/8 + !!(pi->mf_length%8)
#endif
						}
					}
				}
				else if ((err & CollErr) && (!(ReadRawRC(CollReg) & BIT5)))
				{ // a bit-collision is detected
					status = MI_COLLERR;
					if (recebyte)
					{
						val = 0x7F & ReadRawRC(FIFOLevelReg);
						lastBits = ReadRawRC(ControlReg) & 0x07;
						if (len_rest + val > MAX_TRX_BUF_SIZE)
						{ // 长度过长超出缓存
							;
						}
						else
						{
							if (lastBits && val) // 防止spi读错后 val-1成为负值
							{
								pi->mf_length = (val - 1) * 8 + lastBits;
							}
							else
							{
								pi->mf_length = val * 8;
							}
							pi->mf_length += len_rest * 8;
							if (val == 0)
							{
								val = 1;
							}
							for (i = 0; i < val; i++)
							{
								pi->mf_data[len_rest + i + 1] = ReadRawRC(FIFODataReg);
							}
						}
					}
					pi->mf_data[0] = (ReadRawRC(CollReg) & CollPos);
					if (pi->mf_data[0] == 0)
					{
						pi->mf_data[0] = 32;
					}

					pi->mf_data[0]--; // 与之前版本有点映射区别，为了不改变上层代码，这里直接减一；
				}
				else if ((err & CollErr) && (ReadRawRC(CollReg) & BIT5))
				{
					;
				}
				// else if (err & (CrcErr | ParityErr | ProtocolErr))
				else if (err & (ProtocolErr))
				{
					status = MI_FRAMINGERR;
				}
				else if ((err & (CrcErr | ParityErr)) && !(err & ProtocolErr))
				{
					// EMV  parity err EMV 307.2.3.4
					val = 0x7F & ReadRawRC(FIFOLevelReg);
					lastBits = ReadRawRC(ControlReg) & 0x07;
					if (len_rest + val > MAX_TRX_BUF_SIZE)
					{ // 长度过长超出缓存
						status = MI_COM_ERR;
					}
					else
					{
						if (lastBits && val)
						{
							pi->mf_length = (val - 1) * 8 + lastBits;
						}
						else
						{
							pi->mf_length = val * 8;
						}
						pi->mf_length += len_rest * 8;
					}
					status = MI_INTEGRITY_ERR;
				}
				else
				{
					status = MI_INTEGRITY_ERR;
				}
			}
			else
			{
				status = MI_COM_ERR;
			}
		}
		SetBitMask(ControlReg, BIT7);  // TStopNow =1,必要的；
		WriteRawRC(ComIrqReg, 0x7F);   // 清中断0
		WriteRawRC(DivIrqReg, 0x7F);   // 清中断1
		ClearBitMask(ComIEnReg, 0x7F); // 清中断使能,最高位是控制位
		ClearBitMask(DivIEnReg, 0x7F); // 清中断使能,最高位是控制位
		WriteRawRC(CommandReg, PCD_IDLE);
	}
	else
	{
		status = USER_ERROR;
	}
	return status;
}

/**
 ****************************************************************
 * @brief pcd_hlta(void)
 *
 * 功能：命令卡进入休眠状态
 *
 * @param:
 * @return: status 值为MI_OK:成功
 *
 ****************************************************************
 */
unsigned char pcd_hlta(void)
{
	unsigned char status = MI_OK;

	transceive_buffer *pi;
	pi = &mf_com_data;

	WriteRawRC(BitFramingReg, 0x00); // // Tx last bits = 0, rx align = 0
	SetBitMask(TxModeReg, BIT7);	 // 使能发送crc
	ClearBitMask(RxModeReg, BIT7);	 // 不使能接收crc
	PcdSetTmo(2);					 // according to 14443-3 1ms

	mf_com_data.mf_command = PCD_TRANSCEIVE;
	mf_com_data.mf_length = 2;
	mf_com_data.mf_data[0] = PICC_HALT;
	mf_com_data.mf_data[1] = 0;

	status = pcd_com_transceive(pi);

	if (status)
	{
		if (status == MI_NOTAGERR || status == MI_ACCESSTIMEOUT)
		{
#if log_enable
			printf("card enter hlta\n");
#endif
			status = MI_OK;
		}
	}
	return status;
}

/**
 ****************************************************************
 * @brief PcdFastSearchCard(void)
 *
 * 功能：快速寻卡
 *
 * @param:可用来做低功耗，判断场内是否有卡。不做寻uid操作。
 * @param:
 * @return: status 值为MI_OK:成功
 *
 ****************************************************************
 */
unsigned char PcdFastSearch_A_Card(void)
{
#if 1
	unsigned char errmask = 0x20;
	unsigned char errflag;
	unsigned char times;

#if 1
	PcdReset();
#endif
#if log_enable
	printf("PcdFastSearch_A_Card\n");
#endif
	/*output power*/
	 WriteRawRC(0x27,0xFF);      //上电默认88,20
	 WriteRawRC(0x28,0x3F);

	ClearBitMask(Status2Reg, BIT3); // 清零MF crypto1认证成功标记
	WriteRawRC(TxASKReg, 0x40);		// 100%ASK??
	WriteRawRC(ComIrqReg, 0x7F);	// 清中断
	/*********T版芯片打开*****************/
	 unsigned char temp;
		temp = ReadRawRC(0x37);
		if(temp == 0x18)
		{
			WriteRawRC(0x37, 0xA5);
			WriteRawRC(0x32, 0xC9);
			WriteRawRC(0x33, 0x24);
			WriteRawRC(0x37, 0xAE);
			WriteRawRC(0x33, 0xDF);
			WriteRawRC(0x37, 0x5E);
	       	WriteRawRC(0x35, 0xED);
			WriteRawRC(0x3A, 0x10);
	       	WriteRawRC(0x37, temp);
		}
	WriteRawRC(TxControlReg, 0x83);			// 开场
	delay_ms(2);							// delay太小，手机nfc刷不到
	WriteRawRC(FIFODataReg, PICC_REQALL);	// 发寻卡指令：0X26
	WriteRawRC(CommandReg, PCD_TRANSCEIVE); // 启动发送
	WriteRawRC(BitFramingReg, 0x87);		//
	times = 100;								// 查询中断次数，太小容易出现寻不到卡的错误
	do
	{
		errflag = ReadRawRC(ComIrqReg); // 读中断寄存器
		times--;

	} while ((times != 0) && !(errflag & errmask));
	if (times == 0)
	{
		return MI_ERR;
	}
	return MI_OK;
#endif
	// unsigned char status = MI_ERR; //hsf
	// unsigned char atqa[2] = {0}; //hsf
	// unsigned char i = 0; //hsf

	// // PcdReset();
	// PcdConfig('A'); //hsf
	// for (i = 0; i < 3; i++) //hsf
	// {
	// 	status = PcdRequest(PICC_REQALL, atqa); //hsf
	// 	if (status == MI_OK) //hsf
	// 	{
	// 		printf("0x%x 0x%x\n",  atqa[0], atqa[1]);
	// 		return MI_OK; //hsf
	// 	}
	// 	delay_ms(2); //hsf
	// }
	// return MI_ERR; //hsf
}

/**
 ****************************************************************
 * @brief PcdFastSearch_B_Card(void)
 *
 * 功能：快速寻卡
 *
 * @param:可用来做低功耗，判断场内是否有卡。不做寻uid操作。
 * @param:
 * @return: status 值为MI_OK:成功
 *
 ****************************************************************
 */
unsigned char PcdFastSearch_B_Card(void)
{
	unsigned char errmask = 0x20;
	unsigned char errflag;
	unsigned char times;

#if 1
	PcdReset();
#endif
#if log_enable
	printf("PcdFastSearch_B_Card\n");
#endif
	/*output power*/
	//  WriteRawRC(0x27,0x88);
	//  WriteRawRC(0x28,0x20);
	WriteRawRC(Status2Reg, 0x00);  // 清MFCrypto1On
	ClearBitMask(ComIEnReg, BIT7); // 高电平触发中断
	WriteRawRC(ModeReg, 0x3F);	   // CRC seed:FFFF
	WriteRawRC(RxSelReg, 0x88);	   // RxWait
	WriteRawRC(ModGsPReg, 0x12);   // 调制指数，29h值越大，B卡调制深度越小，反之。//B卡调制度要在8-14%
	WriteRawRC(AutoTestReg, 0x00);
	WriteRawRC(TxASKReg, 0x00); // typeB
	WriteRawRC(TypeBReg, 0x13);
	WriteRawRC(TxModeReg, 0x83);	// Tx Framing B
	WriteRawRC(RxModeReg, 0x83);	// Rx framing B
	WriteRawRC(TxControlReg, 0x83); // 开场
	delay_ms(2);					// delay 这个时间也不能小，太小卡片启动不了
	WriteRawRC(FIFODataReg, 0x05);
	WriteRawRC(FIFODataReg, 0x00);
	WriteRawRC(FIFODataReg, 0x08);
	WriteRawRC(ComIrqReg, 0x7F); // Clear INT
	WriteRawRC(DivIrqReg, 0x7F); // Clear INT
	// Flush Fifo
	// SetBitMask(FIFOLevelReg, BIT7);
	WriteRawRC(CommandReg, PCD_TRANSCEIVE); //
	WriteRawRC(BitFramingReg, 0x80);		// 启动发送
	times = 50;								// 次数不能太短，太短中断标志位产生不了
	do
	{
		errflag = ReadRawRC(ComIrqReg); // 等待中断
		times--;
	} while ((times != 0) && !(errflag & errmask));
	if (times == 0)
	{
		printf("PcdFastSearchCard : MI_ERR \n");
		return MI_ERR;
	}
	printf("PcdFastSearchCard : MI_OK \n");
	return MI_OK;
}

/**
 * @brief  寻卡   防冲撞   选卡
 * @param  无
 * @return 无
 */
/**
 ****************************************************************
 * @brief unsigned char ComReqA(unsigned char rw,unsigned char Block)
 *
 * 功能： 寻卡   防冲撞   选卡  14443A_4操作
 *
 * @param:参数rw = READ/WRITE 读写M1卡扇区   参数 Block = M1块绝对地址
 * @return: status 值为MI_OK:成功
 *
 ****************************************************************
 */
unsigned char ComReqA(unsigned char rw, unsigned char Block)
{
	unsigned char status = MI_OK;
	unsigned char sak;

		// status = PcdFastSearch_A_Card();

	PcdConfig('A');							  // 协议配置
	if (PcdRequest(PICC_REQALL, CT) != MI_OK) // 发52指令或者26指令寻场内的卡
		return MI_ERR;
#if log_enable
	user_print_hex("CT : ", CT, 2);
	printf("\n");
#endif
	if (pcd_cascaded_anticoll(PICC_ANTICOLL1, 0, &IDA[0]) != MI_OK) // 一级防碰撞
		return MI_ERR;
	if (status == MI_OK)
	{
		uid_length = 4; // 返回4字节的uid
		status = pcd_cascaded_select(PICC_ANTICOLL1, &IDA[0], &sak);
	}
	if (status == MI_OK && (sak & BIT2)) // 二级防碰撞及选卡
	{
		status = pcd_cascaded_anticoll(PICC_ANTICOLL2, 0, &IDA[4]);
		if (status == MI_OK)
		{
			uid_length = 7; // 返回7字节的uid
			status = pcd_cascaded_select(PICC_ANTICOLL2, &IDA[4], &sak);
		}
	}
	if (status == MI_OK && (sak & BIT2)) // 三级防碰撞及选卡
	{
		status = pcd_cascaded_anticoll(PICC_ANTICOLL3, 0, &IDA[7]);
		if (status == MI_OK)
		{
			uid_length = 10; // 返回10字节的uid
			status = pcd_cascaded_select(PICC_ANTICOLL3, &IDA[7], &sak);
		}
	}

#if ISO14443A_4
	if (sak & 0x20)
	{
#if log_enable
		printf("ISO 14443A_4 Opration :\n");
#endif
		pcd_default_info();				   // 初始化14443A_4参数
		status = com_typea_rats(pps_pcmd); // RATS
										   // APUD();
		/******cpu card read/write  binary file ******/
		status = CpuCard_Write_BinaryFile_Exp();
		// status = CpuCard_Read_BinaryFile_Exp();
		// status = CpuCard_Operation();
	}
#endif

#if M1_CardBlock_ENABLE // 操作扇区
	if (CT[0] == 0x04 && CT[1] == 0x00)
	{
		if (pcd_auth_state(PICC_AUTHENT1A, Block, IDA, PassWd) != MI_OK) // 校验密钥
			return MI_AUTHERR;
		switch (rw)
		{
		case 0:
			status = pcd_read(Block, RWDATA);
			break; // 读数据块
		case 1:
			status = pcd_write(Block, RWDATA);
			break; // 写数据块
		default:
			break;
		}
	}
#endif
	/**************卡片休眠接口****************/
	// pcd_hlta();

	return status;
}

/**
 ****************************************************************
 * @brief ComReqB(void)
 *
 * 功能： 读14443b卡，并返回卡号
 *        B卡卡号 : PUPI    身份证：IDB
 * @param:pcmd  寻感应区内所有符合14443b标准的卡(0x08)
 * @return: status 值为MI_OK:成功
 *
 ****************************************************************
 */
unsigned char ComReqB(void)
{

	unsigned char status = MI_ERR;
	unsigned char i;
	unsigned char cnt;
	unsigned char ATQB[16];
	PcdConfig('B');
	cnt = 1; // 应用中 可以使用轮询N次,轮询场内多张卡片
	uid_length = 0;
	while (cnt--)
	{
		status = PcdRequestB(0x08, 0, 0, ATQB);

		if (status == MI_COLLERR) // 有冲突，超过一张卡
		{
			if ((status = PcdRequestB(0x08, 0, 2, ATQB)) != MI_OK)
			{
				for (i = 1; i < 4; i++)
				{
					if ((status = PcdSlotMarker(i, ATQB)) == MI_OK)
					{
						break;
					}
				}
				if (status == MI_OK)
				{
					break;
				}
			}
			else
			{
				break;
			}
		}
	}

	if (status == MI_OK)
	{
		for (i = 1; i < 5; i++)
		{
			PUPI[i - 1] = *(ATQB + i);
		}
		if (PUPI[0] == 0x00 && PUPI[1] == 0x00 && PUPI[2] == 0x00 && PUPI[3] == 0x00)
		{
			status = PcdAttriB(&ATQB[1], 0, ATQB[10] & 0x0f, PICC_CID, ATQB);
			if (status == MI_OK)
			{
				ATQB[0] = 0x50;	   // 恢复默认值
				GetIdcardNum(IDB); // 获取卡号
				uid_length = 8;
			}
		} // typeB 106默认速率
		else
		{
			uid_length = 4;
		}
	}

	return status;
}
/**
 ****************************************************************
 * @brief ComReqA_Block(void)
 *
 * 功能： M1卡扇区操作接口
 * @param:rw ：读写  block：绝对块号
 * @return: status 值为MI_OK:成功
 *
 ****************************************************************
 */
unsigned char ComReqA_Block(unsigned char rw, unsigned char Block)
{
	unsigned char status = 0;
	if (pcd_auth_state(PICC_AUTHENT1A, Block, IDA, PassWd) != MI_OK)
		return MI_ERR;
	switch (rw)
	{
	case 0:
		status = pcd_read(Block, RWDATA);
		break;
	case 1:
		status = pcd_write(Block, RWDATA);
		break;
	default:
		break;
	}

	return status;
}

/**
 ****************************************************************
 * @brief pcd_auth_state()
 *
 * 功能：用存放在FIFO中的密钥和卡上的密钥进行验证
 *
 * @param: auth_mode=验证方式,0x60:验证A密钥,0x61:验证B密钥
 * @param: block=要验证的绝对块号
 * @param: psnr=序列号首地址
 * @return: status 值为MI_OK:成功
 *
 ****************************************************************
 */
int pcd_auth_state(unsigned char auth_mode, unsigned char block, unsigned char *psnr, unsigned char *pkey)
{
	int status;
	unsigned char i;

	transceive_buffer *pi;
	pi = &mf_com_data;

	WriteRawRC(BitFramingReg, 0x00); // // Tx last bits = 0, rx align = 0

	PcdSetTmo(4);

	mf_com_data.mf_command = PCD_AUTHENT;
	mf_com_data.mf_length = 12;
	mf_com_data.mf_data[0] = auth_mode;
	mf_com_data.mf_data[1] = block;
	for (i = 0; i < 6; i++)
	{
		mf_com_data.mf_data[2 + i] = pkey[i];
	}
	memcpy(&mf_com_data.mf_data[8], psnr, 4);

	status = pcd_com_transceive(pi);

	if (MI_OK == status)
	{
		if (ReadRawRC(Status2Reg) & BIT3) // MFCrypto1On
		{
			status = MI_OK;
#if log_enable
			printf("M1 Auth ok :\n");
#endif
		}
		else
		{
			status = MI_AUTHERR;
		}
	}
	else
	{
#if log_enable
		printf("M1 Auth faid:\n");
#endif
	}

	return status;
}
/////////////////////////////////////////////////////////////////////
// 功    能：读取M1卡一块数据
// 参数说明: addr[IN]：块地址
//           p [OUT]：读出的数据，16字节
// 返    回: 成功返回MI_OK
/////////////////////////////////////////////////////////////////////
/**
 ****************************************************************
 * @brief pcd_read()
 *
 * 功能：读mifare_one卡上一块(block)数据(16字节)
 *
 * @param: addr = 要读的绝对块号
 * @param: preaddata = 存放读出的数据缓存区的首地址
 * @return: status 值为MI_OK:成功
 * @retval: preaddata  读出的数据
 *
 ****************************************************************
 */
int pcd_read(unsigned char addr, unsigned char *preaddata)
{
	int status;
	transceive_buffer *pi;
	pi = &mf_com_data;

	WriteRawRC(BitFramingReg, 0x00); // // Tx last bits = 0, rx align = 0
	SetBitMask(TxModeReg, BIT7);	 // 使能发送CRC
	SetBitMask(RxModeReg, BIT7);	 // 使能接收CRC
	PcdSetTmo(4);

	mf_com_data.mf_command = PCD_TRANSCEIVE;
	mf_com_data.mf_length = 2;
	mf_com_data.mf_data[0] = PICC_READ;
	mf_com_data.mf_data[1] = addr;

	status = pcd_com_transceive(pi);
	if (status == MI_OK)
	{
		if (mf_com_data.mf_length != 0x80)
		{
			status = MI_BITCOUNTERR;
		}
		else
		{
			memcpy(preaddata, &mf_com_data.mf_data[0], 16);
		}
	}
	return status;
}

/**
 ****************************************************************
 * @brief pcd_write()
 *
 * 功能：写数据到M1卡上的一块
 *
 * @param: addr = 要写的绝对块号
 * @param: pwritedata = 存放写的数据缓存区的首地址
 * @return: status 值为MI_OK:成功
 *
 ****************************************************************
 */
int pcd_write(unsigned char addr, unsigned char *pwritedata)
{
	int status;
	transceive_buffer *pi;
	pi = &mf_com_data;

	WriteRawRC(BitFramingReg, 0x00); // // Tx last bits = 0, rx align = 0
	SetBitMask(TxModeReg, BIT7);	 // 使能发送CRC
	ClearBitMask(RxModeReg, BIT7);	 // 不使能接收CRC
	PcdSetTmo(5);

	mf_com_data.mf_command = PCD_TRANSCEIVE;
	mf_com_data.mf_length = 2;
	mf_com_data.mf_data[0] = PICC_WRITE;
	mf_com_data.mf_data[1] = addr;

	status = pcd_com_transceive(pi);
	if (status != MI_NOTAGERR)
	{
		if (mf_com_data.mf_length != 4)
		{
			status = MI_BITCOUNTERR;
		}
		else
		{
			mf_com_data.mf_data[0] &= 0x0F;
			switch (mf_com_data.mf_data[0])
			{
			case 0x00:
				status = MI_NOTAUTHERR;
				break;
			case 0x0A:
				status = MI_OK;
				break;
			default:
				status = MI_CODEERR;
				break;
			}
		}
	}
	if (status == MI_OK)
	{
		PcdSetTmo(5);

		mf_com_data.mf_command = PCD_TRANSCEIVE;
		mf_com_data.mf_length = 16;
		memcpy(&mf_com_data.mf_data[0], pwritedata, 16);

		status = pcd_com_transceive(pi);
		if (status != MI_NOTAGERR)
		{
			mf_com_data.mf_data[0] &= 0x0F;
			switch (mf_com_data.mf_data[0])
			{
			case 0x00:
				status = MI_WRITEERR;
				break;
			case 0x0A:
				status = MI_OK;
				break;
			default:
				status = MI_CODEERR;
				break;
			}
		}
		PcdSetTmo(4);
	}
	return status;
}
/**
 ****************************************************************
 * @brief pcd_write_ultralight()
 *
 * 功能：写数据到卡上的一块
 *
 * @param: addr = 要写的绝对块号
 * @param: pwritedata = 存放写的数据缓存区的首地址
 * @return: status 值为MI_OK:成功
 *
 ****************************************************************
 */
int pcd_write_ultralight(unsigned char addr, unsigned char *pwritedata)
{
	int status;

	transceive_buffer *pi;
	pi = &mf_com_data;

	WriteRawRC(BitFramingReg, 0x00); // // Tx last bits = 0, rx align = 0
	SetBitMask(TxModeReg, BIT7);	 // 使能发送CRC
	ClearBitMask(RxModeReg, BIT7);	 // 不使能接收CRC
	PcdSetTmo(5);

	mf_com_data.mf_command = PCD_TRANSCEIVE;
	mf_com_data.mf_length = 6; // a2h ADR D0 D1 D2 D3
	mf_com_data.mf_data[0] = PICC_WRITE_ULTRALIGHT;
	mf_com_data.mf_data[1] = addr;
	memcpy(&mf_com_data.mf_data[2], pwritedata, 4); // 4 ?????
	status = pcd_com_transceive(pi);

	if (status != MI_NOTAGERR)
	{
		mf_com_data.mf_data[0] &= 0x0F;
		switch (mf_com_data.mf_data[0])
		{
		case 0x00:
			status = MI_WRITEERR;
			break;
		case 0x0A:
			status = MI_OK;
			break;
		default:
			status = MI_CODEERR;
			break;
		}
	}
	PcdSetTmo(4);

	return status;
}

/**
 ****************************************************************
 * @brief pcd_write_ul()
 *
 * 功能：写多个数据
 *
 * @param: addr = 要写的绝对块号
 * @param: pwritedata = 存放写的数据缓存区的首地址
 * @return: status 值为MI_OK:成功
 *
 ****************************************************************
 */
int pcd_write_ul(unsigned char addr, unsigned char *pwritedata, unsigned char inlen)
{
	unsigned char i, j;
	int status = MI_OK;
	unsigned char temp = 0;
	unsigned char n = 0;
	transceive_buffer *pi;
	pi = &mf_com_data;

	n = (inlen / 4 + 1);

	for (i = 0; i < n; i++)
	{
		temp++;
		WriteRawRC(BitFramingReg, 0x00); // // Tx last bits = 0, rx align = 0
		SetBitMask(TxModeReg, BIT7);	 // 使能发送CRC
		ClearBitMask(RxModeReg, BIT7);	 // 不使能接收CRC
		PcdSetTmo(5);

		mf_com_data.mf_command = PCD_TRANSCEIVE;
		mf_com_data.mf_length = 6; // a2h ADR D0 D1 D2 D3
		mf_com_data.mf_data[0] = PICC_WRITE_ULTRALIGHT;
		mf_com_data.mf_data[1] = addr + temp - 1;

		for (j = 0; j < 4; j++)
		{
			mf_com_data.mf_data[2 + j] = pwritedata[j + 4 * (temp - 1)];
		}
		status = pcd_com_transceive(pi);

		if (status != MI_NOTAGERR)
		{
			mf_com_data.mf_data[0] &= 0x0F;
			switch (mf_com_data.mf_data[0])
			{
			case 0x00:
				status = MI_WRITEERR;
				break;
			case 0x0A:
				status = MI_OK;
				break;
			default:
				status = MI_CODEERR;
				break;
			}
		}
	}
	return status;
}

/**
 ****************************************************************
 * @brief pcd_pwd_auth()
 *
 * 功能：校验ultralight秘钥
 *
 * @param: pwd ： 秘钥值
 * @param:
 * @return: status 值为MI_OK:成功
 *
 ****************************************************************
 */
int pcd_pwd_auth(unsigned char *pwd)
{
	unsigned char status;

	transceive_buffer *pi;
	pi = &mf_com_data;

	WriteRawRC(BitFramingReg, 0x00); // // Tx last bits = 0, rx align = 0
	SetBitMask(TxModeReg, BIT7);	 // 使能发送CRC
	SetBitMask(RxModeReg, BIT7);	 // 不使能接收CRC
	PcdSetTmo(5);

	mf_com_data.mf_command = PCD_TRANSCEIVE;
	mf_com_data.mf_length = 5; // a2h ADR D0 D1 D2 D3
	mf_com_data.mf_data[0] = PICC_PWD_AUTH;
	memcpy(&mf_com_data.mf_data[1], pwd, 4); // 4 ?????
	status = pcd_com_transceive(pi);

	if (status == MI_OK)
	{
		user_print_hex("pcd_pwd_auth : ", mf_com_data.mf_data, mf_com_data.mf_length);
	}

	return status;
}
/**
 ****************************************************************
 * @brief pcd_fast_read()
 *
 * 功能：快速读数据
 *
 * @param: startaddr = 起始块号
 * @param: endaddr =  结束块号
 * @param: rdata =  读出到的数据
 * @return: status 值为MI_OK:成功
 *
 ****************************************************************
 */
int pcd_fast_read(unsigned char startaddr, unsigned char endaddr, unsigned char *rdata)
{
	int status;

	transceive_buffer *pi;
	pi = &mf_com_data;

	WriteRawRC(BitFramingReg, 0x00); // // Tx last bits = 0, rx align = 0
	SetBitMask(TxModeReg, BIT7);	 // 使能发送CRC
	// SetBitMask(RxModeReg, BIT7);
	ClearBitMask(RxModeReg, BIT7); // 不使能接收CRC
	PcdSetTmo(5);

	mf_com_data.mf_command = PCD_TRANSCEIVE;
	mf_com_data.mf_length = 3; // a2h ADR D0 D1 D2 D3
	mf_com_data.mf_data[0] = PICC_FAST_READ;
	mf_com_data.mf_data[1] = startaddr;
	mf_com_data.mf_data[2] = endaddr;
	//
	status = pcd_com_transceive(pi);

	if (status == MI_OK)
	{
		user_print_hex("pcd_fast_read : ", mf_com_data.mf_data, 4 * (endaddr - startaddr + 1));
		memcpy(&rdata[0], mf_com_data.mf_data, 4 * (endaddr - startaddr));
	}
	user_print_hex("rdata : ", rdata, 4 * (endaddr - startaddr + 1));
	return status;
}
/**
 ****************************************************************
 * @brief Ntag_Card_Opration()
 *
 * 功能：Ntag卡片读写验证操作示例
 *
 * @param:
 * @param:
 * @param:
 * @return: status 值为MI_OK:成功
 *
 ****************************************************************
 */
unsigned char Ntag_Card_Opration(void)
{
	unsigned char status = MI_OK;

/*  write/read */
#if 1
	status = pcd_fast_read(0x06, 0x0c, fast_readbuff);
	if (status == MI_OK)
	{
		printf("pcd_fast_read ok\n");
	}
	status = pcd_write_ul(0x06, ul_writedata, 18);
	if (status == MI_OK)
	{
		printf("pcd_write_ul ok\n");
	}
#endif

/******************秘钥设置******************/
#if 0
		status = pcd_pwd_auth(Pwd);  //验证秘钥
		if(status == MI_OK)
	{
	  printf("ultralight_pwd_aut ok\n");	
	}
	status = pcd_write_ultralight(PICC_PASS_WORD, Pwd);  //更改秘钥
	if(status == MI_OK)
	{
	  printf("ultralight_write ok\n");
	}
	status = pcd_write_ultralight(PICC_ACCESS, ws_access);   // 设置user操作读写是否需要验证秘钥
	if(status == MI_OK)
	{
	  printf("ultralight_write ok\n");
	}
	status = pcd_write_ultralight(PICC_PACK, pack);  //校验秘钥返回数据
	if(status == MI_OK)
	{
	  printf("ultralight_write PICC_PACK ok\n");
	
	}	
	status = pcd_write_ultralight(PICC_AUTH0, AUTH0);  
	if(status == MI_OK)
	{
	  printf("ultralight_write PICC_AUTH0 ok\n");	
	}
#endif

/**************设置完秘钥验证之后，读写验证***********************/
#if 0
	status = pcd_fast_read(0x06,0x0c,fast_readbuff);
	if(status == MI_OK)
	{
	  printf("pcd_fast_read ok\n");
	}
	status = pcd_write_ultralight(0x06, Pwd);
	if(status == MI_OK)
	{
	  printf("ultralight_write ok\n");
	}
	status = pcd_pwd_auth(Pwd);
		if(status == MI_OK)
	{
	  printf("ultralight_pwd_aut ok\n");
	
	}
	status = pcd_write_ultralight(PICC_PASS_WORD, Pwd);
	if(status == MI_OK)
	{
	  printf("ultralight_write ok\n");
	
	}
	status = pcd_read(0x06,RWDATA);
	if(status == MI_OK)
	{
	   user_print_hex("RWDATA : ", RWDATA ,16);
	  	printf("\n");
	}

#endif

	return status;
}

/**
 ****************************************************************
 * @brief pcd_Value()
 *
 * 功能：增/减值
 *
 * @param: addr = 要增/减值的地址
 * @param: pwritedata = 增/减的四字节值，低位在前
 * @return: status 值为MI_OK:成功
 *
 ****************************************************************
 */
unsigned char pcd_valueblock_operation(unsigned char mode, unsigned char addr, unsigned char *pwritedata)
{
	unsigned char status;

	transceive_buffer *pi;
	pi = &mf_com_data;

#if (NFC_DEBUG)
	printf("VALUE BLOCK OP:\n");
#endif

	WriteRawRC(BitFramingReg, 0x00); // // Tx last bits = 0, rx align = 0
	SetBitMask(TxModeReg, BIT7);	 // 使能发送CRC
	ClearBitMask(RxModeReg, BIT7);	 // 不使能接收crc

	PcdSetTmo(5);
	mf_com_data.mf_command = PCD_TRANSCEIVE;
	mf_com_data.mf_length = 2;
	mf_com_data.mf_data[0] = mode;
	mf_com_data.mf_data[1] = addr;

	status = pcd_com_transceive(pi);
	if (status != MI_NOTAGERR)
	{
		if (mf_com_data.mf_length != 4)
		{
			status = MI_BITCOUNTERR;
		}
		else
		{
			mf_com_data.mf_data[0] &= 0x0F;
			switch (mf_com_data.mf_data[0])
			{
			case 0x00:
				status = MI_NOTAUTHERR;
				break;
			case 0x0A:
				status = MI_OK;
				break;
			default:
				status = MI_CODEERR;
				break;
			}
		}
	}
	if (status == MI_OK)
	{
		PcdSetTmo(5);

		mf_com_data.mf_command = PCD_TRANSCEIVE;
		mf_com_data.mf_length = 4;
		memcpy(&mf_com_data.mf_data[0], pwritedata, 4);

		status = pcd_com_transceive(pi);
		if (status != MI_NOTAGERR)
		{
			mf_com_data.mf_data[0] &= 0x0F;
			switch (mf_com_data.mf_data[0])
			{
			case 0x00:
				status = MI_WRITEERR;
				break;
			case 0x0A:
				status = MI_OK;
				break;
			default:
				status = MI_CODEERR;
				break;
			}
		}
		if (status == MI_NOTAGERR)
			status = MI_OK;
		if (status == MI_OK)
		{
			PcdSetTmo(5);

			mf_com_data.mf_command = PCD_TRANSCEIVE;
			mf_com_data.mf_length = 2;
			mf_com_data.mf_data[0] = PICC_TRANSFER;
			mf_com_data.mf_data[1] = addr;

			status = pcd_com_transceive(pi);

			if (status != MI_NOTAGERR)
			{
				mf_com_data.mf_data[0] &= 0x0F;
				switch (mf_com_data.mf_data[0])
				{
				case 0x00:
					status = MI_WRITEERR;
					break;
				case 0x0A:
					status = MI_OK;
					break;
				default:
					status = MI_CODEERR;
					break;
				}
			}
		}
		PcdSetTmo(4);
	}
	return status;
}
/**
 ****************************************************************
 * @brief pcd_lpcd_start()
 *
 * 功能：lpcd初始化配置
 *
 * @param: delta = 灵敏度设置 （值越大越稳定 0< delta <15 ）
 * @param: swingscnt = 卡探测时间（值越大，探测时间越长，一般也会越稳定，但功耗会增大）
 * @return: 无
 *
 ****************************************************************
 */
void pcd_lpcd_start(unsigned char delta, unsigned char swingscnt)
{
	//
	unsigned int sw;
	sw = swingscnt;
	delta = 0X30 + delta;
#if log_enable
	printf("LPCD START\n");
#endif
	WriteRawRC(0x01, 0x0F); // 寄存器复位
	delay_ms(2);
	WriteRawRC(0x14, 0x8b);	 // 开场
	WriteRawRC(0x37, 0x5e);	 // 打开私有寄存器保护
	WriteRawRC(0x3c, delta); // Delta[3:0]
	// WriteRawRC(0x3d, 0x18);	  //休眠时间	200MS
	WriteRawRC(0x3d, 0x18); // 设置休眠时间	100MS， 0x0d->100ms  0x18->200ms  0x20->250ms   0x40->500ms
#if WS1850S
	WriteRawRC(0x3e, 0x90 | sw); // 设置连续探测次数，探测2次【skip+1】,skip[6:4]， 探测时间  sw = swingscnt，0x97-->14us    0x95-->5.9us
#endif
#if WS1850T
	WriteRawRC(0x3e, 0xa0 | sw); // 设置连续探测次数，探测2次【skip+1】,skip[6:4]， 探测时间  sw = swingscnt，0x97-->14us    0x95-->5.9us
#endif
	WriteRawRC(0x37, 0x00); // 关闭私有寄存器保护开关
#if 1
	WriteRawRC(0x37, 0x5a); // 打开私有寄存器保护开关
	WriteRawRC(0x38, 0x70); // 设置LPCD 发射功率， 高4bit有效     //WS8100_NFC_DEMO_V1.0 1850T: 0x70
	WriteRawRC(0x39, 0x13); // 设置LPCD 发射功率， 低6bit有效     //WS8100_NFC_DEMO_V1.0 1850T: 0x13
	WriteRawRC(0x33, 0x60); // 调整步长,20,60,A0,E0
#if WS1850T
	WriteRawRC(0x3b, 0x82); // 最高位置1，跳过监测时间手动配置使能 [6:0]  2--> 15.6ms  1-->7.8ms
#endif
	WriteRawRC(0x36, 0x80); // ADC参考电平值
	WriteRawRC(0x37, 0x00); // 关闭私有寄存器保护开关
#endif
	ClearBitMask(0x02, 0x80); // 配置IRQ为高电平中断
	// SetBitMask(0x02, 0x80);   //配置IRQ为低电平中断
	WriteRawRC(0x03, 0xA0); // 打开卡探测中断,IRQ 为CMOS 输出
	//	WriteRawRC(0X03,0X20);      //打开卡探测中断,IRQ 为OD 输出
	WriteRawRC(0x01, 0x10); // PCD soft powerdown
	/**********SPI 模式下读取0x31值操作示例*************/
	//  unsigned char i,j;
	//  delay_ms(500);
	//  WriteRawRC(0x37, 0x5a);//打开私有寄存器保护开关
	//	i = ReadRawRC(0x31);
	//	printf("0x31 = %02x",i);
	//	j = ReadRawRC(0x32);
	//	printf("0x32 = %02x",j);
}

static uint8_t nfc_state =0;

uint8_t get_nfc_state(void)
{

	return nfc_state;
}


uint8_t nfc_get_type_a_uid(unsigned char *out_uid, unsigned char *out_len)
{
	const unsigned char *uid_ptr = NULL;
	unsigned char length = uid_length;

	if (out_uid == NULL || out_len == NULL)
	{
		return 0;
	}

	if (nfc_state != 1)
	{
		return 0;
	}

	if (length == 4)
	{
		uid_ptr = &IDA[0];
	}
	else if (length == 7)
	{
		uid_ptr = &IDA[1];
	}
	else if (length == 10)
	{
		uid_ptr = &IDA[0];
	}
	else
	{
		return 0;
	}

	memcpy(out_uid, uid_ptr, length);
	*out_len = length;
	return 1;
}

//****检测卡片类型以及卡片的信息******************************
void Card_Check(void)
{
	unsigned char statusA = MI_OK, statusB = MI_OK;
	unsigned char i;
	nfc_state = 0;


	statusA = ComReqA(READ, Block); // 读取A卡信息
	if (statusA != MI_ERR)
	{
		if (statusA == MI_OK || statusA == MI_AUTHERR || statusA == MI_READERR)
		{
			{
				if (uid_length == 4)
				{
					user_print_hex("IDA : ", &IDA[0], uid_length);
					printf("\n");
					nfc_state =1;
				}
				if (uid_length == 7)
				{
					user_print_hex("IDA : ", &IDA[1], uid_length);
					printf("\n");
					nfc_state =1;
				}
				delay_ms(10);
			}
#if M1_CardBlock_ENABLE
			if (statusA == MI_OK && CT[0] == 0x04 && CT[1] == 0x00)
			{
				
				delay_ms(30);
				user_print_hex("Mifare one read/write : ", RWDATA, 16);
				printf("\n");
				
			}
#endif
		}
	}
	else
	{

		nfc_state =0;
#if TYPE_B_ENABLE
		statusB = ComReqB(); // 读取B卡信息
		if (statusB == MI_OK)
		{
		
			if (uid_length == 8)
			{
				user_print_hex("IDB : ", IDB, uid_length);
				printf("\n");
			}
			if (uid_length == 4)
			{
				user_print_hex("PUPI : ", PUPI, uid_length);
				printf("\n");
			}

			delay_ms(50);

		}
#endif
	}
}

void ws1850_NFC_gpio_init(void) // 引脚初始化（使用可配置引脚）
{
	if (g_nfc_reset_pin != GPIO_NUM_NC) {
		gpio_config_t io_conf = {};
		io_conf.intr_type = GPIO_INTR_DISABLE;
		io_conf.mode = GPIO_MODE_OUTPUT;
		io_conf.pin_bit_mask = (1ULL << g_nfc_reset_pin);
		io_conf.pull_down_en = 0;
		io_conf.pull_up_en = 1;
		gpio_config(&io_conf);
		gpio_set_level(g_nfc_reset_pin, 1);
	}

	if (g_nfc_irq_pin != GPIO_NUM_NC) {
		gpio_config_t io_conf = {};
		io_conf.intr_type = GPIO_INTR_DISABLE;
		io_conf.mode = GPIO_MODE_INPUT;
		io_conf.pin_bit_mask = (1ULL << g_nfc_irq_pin);
		io_conf.pull_down_en = 1;
		io_conf.pull_up_en = 0;
		gpio_config(&io_conf);
	}
}

int nfc_m1_write_block_with_verify(unsigned char block, const unsigned char *pwritedata, unsigned char *pbackup, unsigned char *pverify)
{
	int status;

	if (pwritedata == NULL || pbackup == NULL || pverify == NULL)
	{
		return USER_ERROR;
	}

	/* 禁止写 block 0 和 trailer 块 */
	if (block == 0 || ((block + 1) % 4 == 0))
	{
		return MI_WRONG_ADDR;
	}

	status = ComReqA(READ, block);
	if (status != MI_OK)
	{
		return status;
	}
	if (!(CT[0] == 0x04 && CT[1] == 0x00))
	{
		return MI_ERR;
	}

	memcpy(pbackup, RWDATA, 16); // 备份IC卡数据
	memcpy(RWDATA, pwritedata, 16); // 把数据写入IC卡

	status = ComReqA(WRITE, block);
	if (status != MI_OK)
	{
		return status;
	}

	status = ComReqA(READ, block);
	if (status != MI_OK)
	{
		return status;
	}

	memcpy(pverify, RWDATA, 16);

	// 如果写入和读出来的数据不一致，则返回错误；；；则说明写入失败哦
	if (memcmp(pverify, pwritedata, 16) != 0)
	{
		return MI_WRITEERR;
	}

	return MI_OK;
}
