/********************************** (C) COPYRIGHT *******************************
 * File Name          : ch32v10x_usbfs_device.c
 * Author             : WCH
 * Version            : V1.0.0
 * Date               : 2022/08/20
 * Description        : ch32v10x series usb interrupt Processing.
*********************************************************************************
* Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for 
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/

#include "ch32v10x_usbfs_device.h"

/*******************************************************************************/
/* Variable Definition */
/* Global */
const    uint8_t  *pUSBFS_Descr;

/* Setup Request */
volatile uint8_t  USBFS_SetupReqCode;
volatile uint8_t  USBFS_SetupReqType;
volatile uint16_t USBFS_SetupReqValue;
volatile uint16_t USBFS_SetupReqIndex;
volatile uint16_t USBFS_SetupReqLen;

/* USB Device Status */
volatile uint8_t  USBFS_DevConfig;
volatile uint8_t  USBFS_DevAddr;
volatile uint8_t  USBFS_DevSleepStatus;
volatile uint8_t  USBFS_DevEnumStatus;

/* Endpoint Buffer */
__attribute__ ((aligned(4))) uint8_t USBFS_EP0_Buf[ DEF_USBD_UEP0_SIZE ];
__attribute__ ((aligned(4))) uint8_t USBFS_EP1_Buf[ DEF_USBD_ENDP1_SIZE ];
//__attribute__ ((aligned(4))) uint8_t USBFS_EP2_Buf[ DEF_USBD_ENDP2_SIZE*2 ];

/* USB IN Endpoint Busy Flag */
volatile uint8_t  USBFS_Endp_Busy[ DEF_UEP_NUM ];

/******************************************************************************/
/* Interrupt Service Routine Declaration*/
void USBFS_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));

/*********************************************************************
 * @fn      USBFS_RCC_Init
 *
 * @brief   Set USB clock.
 *
 * @return  none
 */
void USBFS_RCC_Init( void )
{
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    EXTEN->EXTEN_CTR |= EXTEN_USBFS_IO_EN;
    if( SystemCoreClock == 72000000 ) 
    {
        RCC_USBCLKConfig( RCC_USBCLKSource_PLLCLK_1Div5 );
    }
    else if( SystemCoreClock == 48000000 ) 
    {
        RCC_USBCLKConfig( RCC_USBCLKSource_PLLCLK_Div1 );
    }
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_USBFS,ENABLE);
}

/*********************************************************************
 * @fn      USBFS_Device_Endp_Init
 *
 * @brief   Initializes USB device endpoints.
 *
 * @return  none
 */
void USBFS_Device_Endp_Init(void)
{

    R8_UEP4_1_MOD = RB_UEP1_TX_EN;
    R8_UEP2_3_MOD = RB_UEP2_RX_EN | RB_UEP3_TX_EN;

    pEP0_RAM_Addr = USBFS_EP0_Buf;
    R16_UEP0_DMA = (uint16_t)(uint32_t)USBFS_EP0_Buf;
    R16_UEP1_DMA = (uint16_t)(uint32_t)USBFS_EP1_Buf;
    R16_UEP2_DMA = (uint16_t)(uint32_t)UART2_Tx_Buf;
    R16_UEP3_DMA = (uint16_t)(uint32_t)UART2_Rx_Buf;

    R8_UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;

    R8_UEP1_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
    R8_UEP2_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
    R8_UEP3_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;

    /* Clear End-points Busy Status */
    for(uint8_t i=0; i<DEF_UEP_NUM; i++ )
    {
        USBFS_Endp_Busy[ i ] = 0;
    }
}

/*********************************************************************
 * @fn      USBFS_Device_Init
 *
 * @brief   Initializes USB device.
 *
 * @return  none
 */
void USBFS_Device_Init( FunctionalState sta , PWR_VDD VDD_Voltage)
{
    if( VDD_Voltage == PWR_VDD_5V)
    {
        EXTEN->EXTEN_CTR |= EXTEN_USB_5V_SEL;
    }
    else
    {
        EXTEN->EXTEN_CTR &= ~EXTEN_USB_5V_SEL;
    }

    if( sta )
    {
        R8_USB_CTRL = RB_UC_RESET_SIE | RB_UC_CLR_ALL;
        Delay_Us( 10 );
        R8_USB_CTRL = 0x00;
        R8_USB_INT_EN = RB_UIE_SUSPEND | RB_UIE_BUS_RST | RB_UIE_TRANSFER;
        R8_USB_CTRL = RB_UC_DEV_PU_EN | RB_UC_INT_BUSY | RB_UC_DMA_EN;
        USBFS_Device_Endp_Init( );
        R8_UDEV_CTRL = RB_UD_PD_DIS | RB_UD_PORT_EN;
        NVIC_EnableIRQ( USBFS_IRQn );
    }
    else
    {
        R8_USB_CTRL = RB_UC_RESET_SIE | RB_UC_CLR_ALL;
        Delay_Us( 10 );
        R8_USB_CTRL = 0x00;
        NVIC_DisableIRQ( USBFS_IRQn );
    }
}

/*********************************************************************
 * @fn      USBFS_Endp_DataUp
 *
 * @brief   usbfs device data upload
 *          input: endp  - end-point numbers
 *                 *pubf - data buffer
 *                 len   - load data length
 *                 mod   - 0: DEF_UEP_DMA_LOAD 1: DEF_UEP_CPY_LOAD
 *
 * @return  none
 */
uint8_t USBFS_Endp_DataUp(uint8_t endp, uint8_t *pbuf, uint16_t len, uint8_t mod)
{
    uint8_t endp_mode;
    uint8_t buf_load_offset;

    /* DMA config, endp_ctrl config, endp_len config */
    if( (endp>=DEF_UEP1) && (endp<=DEF_UEP7) )
    {
        if( USBFS_Endp_Busy[ endp ] == 0 )
        {
            if( (endp == DEF_UEP1) || (endp == DEF_UEP4) )
            {
                /* endp1/endp4 */
                endp_mode = USBFS_UEP_MOD(0);
                if( endp == DEF_UEP1 )
                {
                    endp_mode = (uint8_t)(endp_mode>>4);
                }
            }
            else if( (endp == DEF_UEP2) || (endp == DEF_UEP3) )
            {
                /* endp2/endp3 */
                endp_mode = USBFS_UEP_MOD(1);
                if( endp == DEF_UEP3 )
                {
                    endp_mode = (uint8_t)(endp_mode>>4);
                }
            }
            else if( (endp == DEF_UEP5) || (endp == DEF_UEP6) )
            {
                /* endp5/endp6 */
                endp_mode = USBFS_UEP_MOD(2);
                if( endp == DEF_UEP6 )
                {
                    endp_mode = (uint8_t)(endp_mode>>4);
                }
            }
            else
            {
                /* endp7 */
                endp_mode = USBFS_UEP_MOD(3);
            }

            if( endp_mode & USBFS_UEP_TX_EN )
            {
                if( endp_mode & USBFS_UEP_RX_EN )
                {
                    buf_load_offset = 64;
                }
                else
                {
                    buf_load_offset = 0;
                }

                if( buf_load_offset == 0 )
                {
                    if( mod == DEF_UEP_DMA_LOAD )
                    {
                        /* DMA mode */
                        USBFS_UEP_DMA(endp) = (uint16_t)(uint32_t)pbuf;
                    }
                    else
                    {
                        /* copy mode */
                        memcpy( USBFS_UEP_BUF(endp), pbuf, len );
                    }
                }
                else
                {
                    memcpy( USBFS_UEP_BUF(endp)+buf_load_offset, pbuf, len );
                }
                /* tx length */
                USBFS_UEP_TLEN(endp) = len;
                /* response ack */
                USBFS_UEP_CTRL(endp) = (USBFS_UEP_CTRL(endp) & ~MASK_UEP_T_RES) | UEP_T_RES_ACK;
                /* Set end-point busy */
                USBFS_Endp_Busy[ endp ] = 0x01;
            }
        }
        else
        {
            return 1;
        }
    }
    else
    {
        return 1;
    }
    return 0;
}

/*********************************************************************
 * @fn      USBFS_IRQHandler
 *
 * @brief   This function handles USB FS exception.
 *
 * @return  none
 */
void USBFS_IRQHandler( void )
{
    uint8_t  intflag, intst, errflag;
    uint16_t len;

    intflag = R8_USB_INT_FG;
    intst = R8_USB_INT_ST;

    if( intflag & RB_UIF_TRANSFER )
    {
        switch ( intst & MASK_UIS_TOKEN )
        {
            /* data-in stage processing */
            case UIS_TOKEN_IN:
                switch ( intst & ( MASK_UIS_TOKEN | MASK_UIS_ENDP ) )
                {
                    /* end-point 0 data in interrupt */
                    case UIS_TOKEN_IN | DEF_UEP0:
                        if( USBFS_SetupReqLen == 0 )
                        {
                            R8_UEP0_CTRL = (R8_UEP0_CTRL & ~MASK_UEP_R_RES ) | UEP_R_RES_ACK;
                        }
                        if ( ( USBFS_SetupReqType & USB_REQ_TYP_MASK ) != USB_REQ_TYP_STANDARD )
                        {
                            /* Non-standard request endpoint 0 Data upload */
                        }
                        else
                        {
                            switch( USBFS_SetupReqCode )
                            {
                                case USB_GET_DESCRIPTOR:
                                        len = USBFS_SetupReqLen >= DEF_USBD_UEP0_SIZE ? DEF_USBD_UEP0_SIZE : USBFS_SetupReqLen;
                                        memcpy( USBFS_EP0_Buf, pUSBFS_Descr, len );
                                        USBFS_SetupReqLen -= len;
                                        pUSBFS_Descr += len;
                                        R8_UEP0_T_LEN   = len;
                                        R8_UEP0_CTRL ^= RB_UEP_T_TOG;
                                        break;

                                case USB_SET_ADDRESS:
                                        R8_USB_DEV_AD = ( R8_USB_DEV_AD & RB_UDA_GP_BIT ) | USBFS_DevAddr;
                                        break;

                                default:
                                        break;
                            }
                        }
                        break;

                        /* end-point 1 data in interrupt */
                        case UIS_TOKEN_IN | DEF_UEP1:
                            R8_UEP1_CTRL = (R8_UEP1_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_NAK;
                            R8_UEP1_CTRL ^= RB_UEP_T_TOG;
                            USBFS_Endp_Busy[ DEF_UEP1 ] = 0;
                            break;

                        /* end-point 2 data in interrupt */
                        case UIS_TOKEN_IN | DEF_UEP3:
                            R8_UEP3_CTRL = (R8_UEP3_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_NAK;
                            R8_UEP3_CTRL ^= RB_UEP_T_TOG;
                            USBFS_Endp_Busy[ DEF_UEP3 ] = 0;
                            Uart.USB_Up_IngFlag = 0x00;
                            break;

                    default :
                        break;
                }
                break;

            /* data-out stage processing */
            case UIS_TOKEN_OUT:
                switch ( intst & ( MASK_UIS_TOKEN | MASK_UIS_ENDP ) )
                {
                    /* end-point 0 data out interrupt */
                    case UIS_TOKEN_OUT | DEF_UEP0:
                        len = (uint16_t)(R16_USB_RX_LEN & MASK_UIS_RX_LEN);;
                        if ( intst & RB_UIS_TOG_OK )
                        {
                            if ( ( USBFS_SetupReqType & USB_REQ_TYP_MASK ) != USB_REQ_TYP_STANDARD )
                            {
                                /* Non-standard request end-point 0 Data download */
                                USBFS_SetupReqLen = 0;
                                /* Non-standard request end-point 0 Data download */
                                if( USBFS_SetupReqCode == CDC_SET_LINE_CODING )
                                {
                                      /* Save relevant parameters such as serial port baud rate */
                                      /* The downlinked data is processed in the endpoint 0 OUT packet, the 7 bytes of the downlink are, in order
                                         4 bytes: baud rate value: lowest baud rate byte, next lowest baud rate byte, next highest baud rate byte, highest baud rate byte.
                                         1 byte: number of stop bits (0: 1 stop bit; 1: 1.5 stop bit; 2: 2 stop bits).
                                         1 byte: number of parity bits (0: None; 1: Odd; 2: Even; 3: Mark; 4: Space).
                                         1 byte: number of data bits (5,6,7,8,16); */
                                      Uart.Com_Cfg[ 0 ] = USBFS_EP0_Buf[ 0 ];
                                      Uart.Com_Cfg[ 1 ] = USBFS_EP0_Buf[ 1 ];
                                      Uart.Com_Cfg[ 2 ] = USBFS_EP0_Buf[ 2 ];
                                      Uart.Com_Cfg[ 3 ] = USBFS_EP0_Buf[ 3 ];
                                      Uart.Com_Cfg[ 4 ] = USBFS_EP0_Buf[ 4 ];
                                      Uart.Com_Cfg[ 5 ] = USBFS_EP0_Buf[ 5 ];
                                      Uart.Com_Cfg[ 6 ] = USBFS_EP0_Buf[ 6 ];
                                      Uart.Com_Cfg[ 7 ] = DEF_UARTx_RX_TIMEOUT;
                                      UART2_USB_Init( );
                                 }
                            }
                            else
                            {
                                /* Standard request end-point 0 Data download */
                                /* Add your code here */
                            }
                            if( USBFS_SetupReqLen == 0 )
                            {
                                R8_UEP0_T_LEN = 0;
                                R8_UEP0_CTRL = (R8_UEP0_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_ACK | RB_UEP_T_TOG;
                            }
                        }
                        break;

                    /* end-point 1 data out interrupt */
                    case UIS_TOKEN_OUT | DEF_UEP2:
                        R8_UEP2_CTRL ^= RB_UEP_R_TOG;
                        Uart.Tx_PackLen[ Uart.Tx_LoadNum ] = (uint16_t)( R16_USB_RX_LEN&MASK_UIS_RX_LEN );
                        Uart.Tx_LoadNum++;
                        R16_UEP2_DMA = (uint16_t)(uint32_t)(uint8_t *)&UART2_Tx_Buf[ ( Uart.Tx_LoadNum * DEF_USB_FS_PACK_LEN ) ];

                        if( Uart.Tx_LoadNum >= DEF_UARTx_TX_BUF_NUM_MAX )
                        {
                            Uart.Tx_LoadNum = 0x00;
                            R16_UEP2_DMA = (uint16_t)(uint32_t)(uint8_t *)&UART2_Tx_Buf[ 0 ];
                        }
                        Uart.Tx_RemainNum++;
                        if( Uart.Tx_RemainNum >= ( DEF_UARTx_TX_BUF_NUM_MAX - 2 ) )
                        {
                            R8_UEP2_CTRL = (R8_UEP2_CTRL & ~MASK_UEP_R_RES) | UEP_R_RES_NAK;
                            Uart.USB_Down_StopFlag = 0x01;
                        }
                        break;

                    default:
                        break;
                }
                break;

            /* Setup stage processing */
            case UIS_TOKEN_SETUP:
                R8_UEP0_CTRL = RB_UEP_R_TOG | RB_UEP_T_TOG | UEP_R_RES_NAK | UEP_T_RES_NAK;
                /* Store All Setup Values */
                USBFS_SetupReqType  = pUSBFS_SetupReqPak->bRequestType;
                USBFS_SetupReqCode  = pUSBFS_SetupReqPak->bRequest;
                USBFS_SetupReqLen   = pUSBFS_SetupReqPak->wLength;
                USBFS_SetupReqValue = pUSBFS_SetupReqPak->wValue;
                USBFS_SetupReqIndex = pUSBFS_SetupReqPak->wIndex;
                len = 0;
                errflag = 0;
                if ( ( USBFS_SetupReqType & USB_REQ_TYP_MASK ) != USB_REQ_TYP_STANDARD )
                {
                    /* usb non-standard request processing */
                    /* errflag = 0xFF; if this request or cmd dose not support */
                    /* usb non-standard request processing */
                       if( USBFS_SetupReqType & USB_REQ_TYP_CLASS )
                       {
                           /* Class requests */
                           switch( USBFS_SetupReqCode )
                           {
                               case CDC_GET_LINE_CODING:
                                   pUSBFS_Descr = (uint8_t *)&Uart.Com_Cfg[ 0 ];
                                   len = 7;
                                   break;

                               case CDC_SET_LINE_CODING:
                                   break;

                               case CDC_SET_LINE_CTLSTE:
                                   break;

                               case CDC_SEND_BREAK:
                                   break;

                               default:
                                   errflag = 0xff;
                                   break;
                           }
                       }
                       else if( USBFS_SetupReqType & USB_REQ_TYP_VENDOR )
                       {
                           /* Manufacturer request */
                       }
                       else
                       {
                           errflag = 0xFF;
                       }

                       /* Copy Descriptors to Endp0 DMA buffer */
                       len = (USBFS_SetupReqLen >= DEF_USBD_UEP0_SIZE) ? DEF_USBD_UEP0_SIZE : USBFS_SetupReqLen;
                       memcpy( USBFS_EP0_Buf, pUSBFS_Descr, len );
                       pUSBFS_Descr += len;
                }
                else
                {
                    /* usb standard request processing */
                    switch( USBFS_SetupReqCode )
                    {
                        /* get device/configuration/string/report/... descriptors */
                        case USB_GET_DESCRIPTOR:
                            switch( (uint8_t)( USBFS_SetupReqValue >> 8 ) )
                            {
                                /* get usb device descriptor */
                                case USB_DESCR_TYP_DEVICE:
                                    pUSBFS_Descr = MyDevDescr;
                                    len = DEF_USBD_DEVICE_DESC_LEN;
                                    break;

                                /* get usb configuration descriptor */
                                case USB_DESCR_TYP_CONFIG:
                                    pUSBFS_Descr = MyCfgDescr;
                                    len = DEF_USBD_CONFIG_DESC_LEN;
                                    break;

                                /* get usb string descriptor */
                                case USB_DESCR_TYP_STRING:
                                    switch( (uint8_t)( USBFS_SetupReqValue & 0xFF ) )
                                    {
                                        /* Descriptor 0, Language descriptor */
                                        case DEF_STRING_DESC_LANG:
                                            pUSBFS_Descr = MyLangDescr;
                                            len = DEF_USBD_LANG_DESC_LEN;
                                            break;

                                        /* Descriptor 1, Manufacturers String descriptor */
                                        case DEF_STRING_DESC_MANU:
                                            pUSBFS_Descr = MyManuInfo;
                                            len = DEF_USBD_MANU_DESC_LEN;
                                            break;

                                        /* Descriptor 2, Product String descriptor */
                                        case DEF_STRING_DESC_PROD:
                                            pUSBFS_Descr = MyProdInfo;
                                            len = DEF_USBD_PROD_DESC_LEN;
                                            break;

                                        /* Descriptor 3, Serial-number String descriptor */
                                        case DEF_STRING_DESC_SERN:
                                            pUSBFS_Descr = MySerNumInfo;
                                            len = DEF_USBD_SN_DESC_LEN;
                                            break;

                                        default:
                                            errflag = 0xFF;
                                            break;
                                    }
                                    break;

                                default :
                                    errflag = 0xFF;
                                    break;
                            }

                            /* Copy Descriptors to Endp0 DMA buffer */
                            if( USBFS_SetupReqLen>len )
                            {
                                USBFS_SetupReqLen = len;
                            }
                            len = (USBFS_SetupReqLen >= DEF_USBD_UEP0_SIZE) ? DEF_USBD_UEP0_SIZE : USBFS_SetupReqLen;
                            memcpy( USBFS_EP0_Buf, pUSBFS_Descr, len );
                            pUSBFS_Descr += len;
                            break;

                        /* Set usb address */
                        case USB_SET_ADDRESS:
                            USBFS_DevAddr = (uint8_t)( USBFS_SetupReqValue & 0xFF );
                            break;

                        /* Get usb configuration now set */
                        case USB_GET_CONFIGURATION:
                            USBFS_EP0_Buf[0] = USBFS_DevConfig;
                            if ( USBFS_SetupReqLen > 1 )
                            {
                                USBFS_SetupReqLen = 1;
                            }
                            break;

                        /* Set usb configuration to use */
                        case USB_SET_CONFIGURATION:
                            USBFS_DevConfig = (uint8_t)( USBFS_SetupReqValue & 0xFF );
                            USBFS_DevEnumStatus = 0x01;
                            break;

                        /* Clear or disable one usb feature */
                        case USB_CLEAR_FEATURE:
                            if ( ( USBFS_SetupReqType & USB_REQ_RECIP_MASK ) == USB_REQ_RECIP_DEVICE )
                            {
                                /* clear one device feature */
                                if( (uint8_t)( USBFS_SetupReqValue & 0xFF ) == USB_REQ_FEAT_REMOTE_WAKEUP )
                                {
                                    /* clear usb sleep status, device not prepare to sleep */
                                    USBFS_DevSleepStatus &= ~0x01;
                                }
                            }
                            else if( ( USBFS_SetupReqType & USB_REQ_RECIP_MASK ) == USB_REQ_RECIP_ENDP )
                            {
                                /* Clear End-point Feature */
                                if( (uint8_t)( USBFS_SetupReqValue & 0xFF ) == USB_REQ_FEAT_ENDP_HALT )
                                {
                                    switch( (uint8_t)( USBFS_SetupReqIndex & 0xFF ) )
                                    {
                                        case ( DEF_UEP_IN | DEF_UEP1 ):
                                            /* Set End-point 1 IN NAK */
                                            R8_UEP1_CTRL = ( R8_UEP1_CTRL & ~( RB_UEP_T_TOG | MASK_UEP_T_RES ) ) | UEP_T_RES_NAK;
                                            break;

                                        case ( DEF_UEP_OUT | DEF_UEP2 ):
                                            /* Set End-point 2 OUT ACK */
                                            R8_UEP2_CTRL = ( R8_UEP2_CTRL & ~( RB_UEP_R_TOG | MASK_UEP_R_RES ) ) | UEP_R_RES_ACK;
                                            break;

                                        case ( DEF_UEP_IN | DEF_UEP3 ):
                                            /* Set End-point 3 IN NAK */
                                            R8_UEP3_CTRL = ( R8_UEP3_CTRL & ~( RB_UEP_T_TOG | MASK_UEP_T_RES ) ) | UEP_T_RES_NAK;
                                            break;

                                        default:
                                            errflag = 0xFF;
                                            break;
                                    }
                                }
                                else
                                {
                                    errflag = 0xFF;
                                }
                            }
                            else
                            {
                                errflag = 0xFF;
                            }
                            break;

                        /* set or enable one usb feature */
                        case USB_SET_FEATURE:
                            if( ( USBFS_SetupReqType & USB_REQ_RECIP_MASK ) == USB_REQ_RECIP_DEVICE )
                            {
                                /* Set Device Feature */
                                if( (uint8_t)( USBFS_SetupReqValue & 0xFF ) == USB_REQ_FEAT_REMOTE_WAKEUP )
                                {
                                    if( MyCfgDescr[ 7 ] & 0x20 )
                                    {
                                        /* Set Wake-up flag, device prepare to sleep */
                                        USBFS_DevSleepStatus |= 0x01;
                                    }
                                    else
                                    {
                                        errflag = 0xFF;
                                    }
                                }
                                else
                                {
                                    errflag = 0xFF;
                                }
                            }
                            else if( ( USBFS_SetupReqType & USB_REQ_RECIP_MASK ) == USB_REQ_RECIP_ENDP )
                            {
                                /* Set End-point Feature */
                                if( (uint8_t)( USBFS_SetupReqValue & 0xFF ) == USB_REQ_FEAT_ENDP_HALT )
                                {
                                    /* Set end-points status stall */
                                    switch( (uint8_t)( USBFS_SetupReqIndex & 0xFF ) )
                                    {
                                        case ( DEF_UEP_IN | DEF_UEP1 ):
                                            /* Set End-point 1 IN STALL */
                                            R8_UEP1_CTRL = ( R8_UEP1_CTRL & ~( RB_UEP_T_TOG | MASK_UEP_T_RES ) ) | UEP_T_RES_STALL;
                                            break;

                                        case ( DEF_UEP_OUT | DEF_UEP2 ):
                                            /* Set End-point 2 OUT STALL */
                                            R8_UEP2_CTRL = ( R8_UEP2_CTRL & ~( RB_UEP_R_TOG | MASK_UEP_R_RES ) ) | UEP_R_RES_STALL;
                                            break;

                                        case ( DEF_UEP_IN | DEF_UEP3 ):
                                            /* Set End-point 3 IN STALL */
                                            R8_UEP3_CTRL = ( R8_UEP3_CTRL & ~( RB_UEP_T_TOG | MASK_UEP_T_RES ) ) | UEP_T_RES_STALL;
                                            break;

                                        default:
                                            errflag = 0xFF;
                                            break;
                                    }
                                }
                                else
                                {
                                    errflag = 0xFF;
                                }
                            }
                            else
                            {
                                errflag = 0xFF;
                            }
                            break;

                        /* This request allows the host to select another setting for the specified interface  */
                        case USB_GET_INTERFACE:
                            USBFS_EP0_Buf[0] = 0x00;
                            if ( USBFS_SetupReqLen > 1 )
                            {
                                USBFS_SetupReqLen = 1;
                            }
                            break;

                        case USB_SET_INTERFACE:
                            break;

                        /* host get status of specified device/interface/end-points */
                        case USB_GET_STATUS:
                            USBFS_EP0_Buf[ 0 ] = 0x00;
                            USBFS_EP0_Buf[ 1 ] = 0x00;
                            if ( ( USBFS_SetupReqType & USB_REQ_RECIP_MASK ) == USB_REQ_RECIP_DEVICE )
                            {
                                if( USBFS_DevSleepStatus & 0x01 )
                                {
                                    USBFS_EP0_Buf[ 0 ] = 0x02;
                                }
                            }
                            else if( ( USBFS_SetupReqType & USB_REQ_RECIP_MASK ) == USB_REQ_RECIP_ENDP )
                            {
                                switch( (uint8_t)( USBFS_SetupReqIndex & 0xFF ) )
                                {
                                    case ( DEF_UEP_IN | DEF_UEP1 ):
                                        if( ( R8_UEP1_CTRL & MASK_UEP_T_RES ) == UEP_T_RES_STALL )
                                        {
                                            USBFS_EP0_Buf[ 0 ] = 0x01;
                                        }
                                        break;

                                    case ( DEF_UEP_OUT | DEF_UEP2 ):
                                        if( ( R8_UEP2_CTRL & MASK_UEP_R_RES ) == UEP_R_RES_STALL )
                                        {
                                            USBFS_EP0_Buf[ 0 ] = 0x01;
                                        }
                                        break;

                                    case ( DEF_UEP_IN | DEF_UEP3 ):
                                        if( ( R8_UEP3_CTRL & MASK_UEP_T_RES ) == UEP_T_RES_STALL )
                                        {
                                            USBFS_EP0_Buf[ 0 ] = 0x01;
                                        }
                                        break;

                                    default:
                                        errflag = 0xFF;
                                        break;
                                }
                            }
                            else
                            {
                                errflag = 0xFF;
                            }

                            if ( USBFS_SetupReqLen > 2 )
                            {
                                USBFS_SetupReqLen = 2;
                            }

                            break;

                        default:
                            errflag = 0xFF;
                            break;
                    }
                }

                /* errflag = 0xFF means a request not support or some errors occurred, else correct */
                if( errflag == 0xFF)
                {
                    /* if one request not support, return stall */
                    R8_UEP0_CTRL = RB_UEP_R_TOG | RB_UEP_T_TOG | UEP_R_RES_STALL | UEP_T_RES_STALL;
                }
                else
                {
                    /* end-point 0 data Tx/Rx */
                    if( USBFS_SetupReqType & DEF_UEP_IN )
                    {
                        /* tx */
                        len = ( USBFS_SetupReqLen > DEF_USBD_UEP0_SIZE ) ? DEF_USBD_UEP0_SIZE : USBFS_SetupReqLen;
                        USBFS_SetupReqLen -= len;
                        R8_UEP0_T_LEN  = len;
                        R8_UEP0_CTRL = (R8_UEP0_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_ACK;
                    }
                    else
                    {
                        /* rx */
                        if( USBFS_SetupReqLen == 0 )
                        {
                            R8_UEP0_T_LEN = 0;
                            R8_UEP0_CTRL = (R8_UEP0_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_ACK;
                        }
                        else
                        {
                            R8_UEP0_CTRL = (R8_UEP0_CTRL & ~MASK_UEP_R_RES) | UEP_R_RES_ACK ;
                        }
                    }
                }
                break;

            /* Sof pack processing */
            case UIS_TOKEN_SOF:
                break;

            default :
                break;
        }
        R8_USB_INT_FG = RB_UIF_TRANSFER;
    }
    else if( intflag & RB_UIF_BUS_RST )
    {
        /* usb reset interrupt processing */
        USBFS_DevConfig = 0;
        USBFS_DevAddr = 0;
        USBFS_DevSleepStatus = 0;
        USBFS_DevEnumStatus = 0;

        R8_USB_DEV_AD = 0;
        USBFS_Device_Endp_Init( );
        UART2_ParaInit( 1 );
        R8_USB_INT_FG |= RB_UIF_BUS_RST;
    }
    else if( intflag & RB_UIF_SUSPEND )
    {
        /* usb suspend interrupt processing */
        R8_USB_INT_FG = RB_UIF_SUSPEND;
        Delay_Us(10);
        if ( R8_USB_MIS_ST & RB_UMS_SUSPEND )
        {
            USBFS_DevSleepStatus |= 0x02;
            if( USBFS_DevSleepStatus == 0x03 )
            {
                /* Handling usb sleep here */
            }
        }
        else
        {
            USBFS_DevSleepStatus &= ~0x02;
        }
    }
    else
    {
        /* other interrupts */
        R8_USB_INT_FG = intflag;
    }
}

/*********************************************************************
 * @fn      USBFS_Send_Resume
 *
 * @brief   USBFS device sends wake-up signal to host
 *
 * @return  none
 */
void USBFS_Send_Resume(void)
{
    R8_UDEV_CTRL ^= RB_UD_LOW_SPEED;
    Delay_Ms(8);
    R8_UDEV_CTRL ^= RB_UD_LOW_SPEED;
}
