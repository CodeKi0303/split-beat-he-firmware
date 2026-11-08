/* ST USB Device Core <-> STM32H723 USB OTG HS, external USB3300 ULPI PHY.
 * GPIO reset and 24 MHz XO startup belong to board_init() before USB init.
 * No CDC, internal FS PHY, dynamic allocation, or USB DMA. */
#include "usb_internal.h"
#include "stm32h7xx_hal.h"
#if defined(C86_CPU_PROFILE)
#include "cpu_profile.h"
#endif
#include <stdbool.h>

PCD_HandleTypeDef hpcd_USB_OTG_HS;
static USBD_StatusTypeDef status(HAL_StatusTypeDef s)
{ return s == HAL_OK ? USBD_OK : s == HAL_BUSY ? USBD_BUSY : USBD_FAIL; }

static bool wait_clear(volatile uint32_t *reg, uint32_t mask)
{
    /* Count-bounded even if SysTick is masked in USB IRQ priority 5. Higher
     * priority ADC and split IRQs remain enabled throughout these handshakes. */
    for (unsigned n = 0; n < 2048U; ++n) if ((*reg & mask) == 0U) return true;
    return false;
}
static HAL_StatusTypeDef quiesce_in(PCD_HandleTypeDef *h, uint8_t ep)
{
    uint32_t USBx_BASE = (uint32_t)h->Instance;
    USB_OTG_INEndpointTypeDef *in = USBx_INEP(ep);
    /* FIFO flushing alone leaves EPENA and a pending transfer active. Stop
     * refill interrupts and disable the transfer before clearing its FIFO.
     * Deliberately preserve DATA toggle state across suspend/resume. */
    USBx_DEVICE->DIEPEMPMSK &= ~(1UL << ep);
    if (in->DIEPCTL & USB_OTG_DIEPCTL_EPENA) {
        in->DIEPCTL |= USB_OTG_DIEPCTL_SNAK;
        in->DIEPCTL |= USB_OTG_DIEPCTL_EPDIS;
        if (!wait_clear(&in->DIEPCTL, USB_OTG_DIEPCTL_EPENA)) goto failed;
    }
    for (unsigned n = 0; (h->Instance->GRSTCTL & USB_OTG_GRSTCTL_AHBIDL) == 0U; ++n)
        if (n == 2048U) goto failed;
    h->Instance->GRSTCTL = USB_OTG_GRSTCTL_TXFFLSH | ((uint32_t)ep << 6);
    if (!wait_clear(&h->Instance->GRSTCTL, USB_OTG_GRSTCTL_TXFFLSH)) goto failed;
    in->DIEPINT = 0xFB7FU; /* W1C pending completion/disable, as in ST StopDevice. */
    h->IN_ep[ep].xfer_buff = NULL;
    h->IN_ep[ep].xfer_len = h->IN_ep[ep].xfer_count = 0;
    return HAL_OK;
failed:
    /* No stalled USB peripheral can keep a pressed keyboard report alive.
     * Reinitialization is deferred to usb_device_poll(), after detach time. */
    (void)USB_DevDisconnect(h->Instance);
    usb_device_transport_failed();
    return HAL_ERROR;
}

void HAL_PCD_MspInit(PCD_HandleTypeDef *pcd)
{
    if (pcd->Instance != USB_OTG_HS) return;
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    /* C77 LQFP100 exposes PC2_C/PC3_C: close the switches to connect the
     * digital PC2/PC3 ULPI functions. OPEN disconnects those digital paths. */
    HAL_SYSCFG_AnalogSwitchConfig(SYSCFG_SWITCH_PC2, SYSCFG_SWITCH_PC2_CLOSE);
    HAL_SYSCFG_AnalogSwitchConfig(SYSCFG_SWITCH_PC3, SYSCFG_SWITCH_PC3_CLOSE);
    GPIO_InitTypeDef gpio = {0};
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = 10;
    gpio.Pin = GPIO_PIN_3 | GPIO_PIN_5; /* D0, CLK */
    HAL_GPIO_Init(GPIOA, &gpio);
    gpio.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_5 | GPIO_PIN_10 |
               GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_13; /* D1..D7 */
    HAL_GPIO_Init(GPIOB, &gpio);
    gpio.Pin = GPIO_PIN_0 | GPIO_PIN_2 | GPIO_PIN_3; /* STP, DIR, NXT */
    HAL_GPIO_Init(GPIOC, &gpio);
    HAL_PWREx_EnableUSBVoltageDetector();
    __HAL_RCC_USB_OTG_HS_CLK_ENABLE();
    __HAL_RCC_USB_OTG_HS_ULPI_CLK_ENABLE();
    HAL_NVIC_SetPriority(OTG_HS_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(OTG_HS_IRQn);
}
void HAL_PCD_MspDeInit(PCD_HandleTypeDef *pcd)
{
    if (pcd->Instance != USB_OTG_HS) return;
    HAL_NVIC_DisableIRQ(OTG_HS_IRQn);
    __HAL_RCC_USB_OTG_HS_ULPI_CLK_DISABLE();
    __HAL_RCC_USB_OTG_HS_CLK_DISABLE();
    HAL_GPIO_DeInit(GPIOA, GPIO_PIN_3 | GPIO_PIN_5);
    HAL_GPIO_DeInit(GPIOB, GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_5 | GPIO_PIN_10 |
                   GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_13);
    HAL_GPIO_DeInit(GPIOC, GPIO_PIN_0 | GPIO_PIN_2 | GPIO_PIN_3);
}
void OTG_HS_IRQHandler(void)
{
#if defined(C86_CPU_PROFILE)
    CPU_PROFILE_BEGIN(cpu_irq);
#endif
    HAL_PCD_IRQHandler(&hpcd_USB_OTG_HS);
#if defined(C86_CPU_PROFILE)
    CPU_PROFILE_IRQ_END(cpu_irq,CPU_PROF_IRQ_USB);
#endif
}
void HAL_PCD_SetupStageCallback(PCD_HandleTypeDef *h)
{ usb_device_setup_begin(); USBD_LL_SetupStage(h->pData, (uint8_t *)h->Setup); }
void HAL_PCD_DataOutStageCallback(PCD_HandleTypeDef *h, uint8_t ep)
{ USBD_LL_DataOutStage(h->pData, ep, h->OUT_ep[ep].xfer_buff); }
void HAL_PCD_DataInStageCallback(PCD_HandleTypeDef *h, uint8_t ep)
{ USBD_LL_DataInStage(h->pData, ep, h->IN_ep[ep].xfer_buff); }
void HAL_PCD_SOFCallback(PCD_HandleTypeDef *h) { USBD_LL_SOF(h->pData); }
void HAL_PCD_ResetCallback(PCD_HandleTypeDef *h)
{
    /* HAL updates Init.speed from ENUMDNE, not the requested initial speed. */
    usb_device_bus_event();
    USBD_LL_SetSpeed(h->pData, h->Init.speed == PCD_SPEED_HIGH ? USBD_SPEED_HIGH : USBD_SPEED_FULL);
    USBD_LL_Reset(h->pData);
}
void HAL_PCD_SuspendCallback(PCD_HandleTypeDef *h)
{
    /* Prevent a queued pressed report or completion reappearing after resume. */
    for (uint8_t ep = 1; ep <= 4; ++ep)
        if (quiesce_in(h, ep) != HAL_OK) break;
    usb_device_bus_event();
    USBD_LL_Suspend(h->pData);
}
void HAL_PCD_ResumeCallback(PCD_HandleTypeDef *h)
{ USBD_LL_Resume(h->pData); }
void HAL_PCD_ConnectCallback(PCD_HandleTypeDef *h)
{ USBD_LL_DevConnected(h->pData); }
void HAL_PCD_DisconnectCallback(PCD_HandleTypeDef *h)
{ usb_device_bus_event(); USBD_LL_DevDisconnected(h->pData); }
void HAL_PCD_ISOOUTIncompleteCallback(PCD_HandleTypeDef *h, uint8_t ep)
{ USBD_LL_IsoOUTIncomplete(h->pData, ep); }
void HAL_PCD_ISOINIncompleteCallback(PCD_HandleTypeDef *h, uint8_t ep)
{ USBD_LL_IsoINIncomplete(h->pData, ep); }

USBD_StatusTypeDef USBD_LL_Init(USBD_HandleTypeDef *dev)
{
    if (dev->id != DEVICE_HS) return USBD_FAIL;
    hpcd_USB_OTG_HS.pData = dev;
    dev->pData = &hpcd_USB_OTG_HS;
    hpcd_USB_OTG_HS.Instance = USB_OTG_HS;
    hpcd_USB_OTG_HS.Init.dev_endpoints = 5;
    hpcd_USB_OTG_HS.Init.speed = PCD_SPEED_HIGH;
    hpcd_USB_OTG_HS.Init.dma_enable = DISABLE;
    hpcd_USB_OTG_HS.Init.phy_itface = USB_OTG_ULPI_PHY;
    hpcd_USB_OTG_HS.Init.Sof_enable = DISABLE;
    hpcd_USB_OTG_HS.Init.low_power_enable = DISABLE;
    hpcd_USB_OTG_HS.Init.lpm_enable = DISABLE;
    /* C77 feeds host VBUS sense into USB3300; external PHY reports sessions. */
    hpcd_USB_OTG_HS.Init.vbus_sensing_enable = ENABLE;
    hpcd_USB_OTG_HS.Init.use_dedicated_ep1 = DISABLE;
    /* EXTVBUS/CPEN are NC; use USB3300's own VBUS comparator, not an external
     * comparator/drive path. PHY pin4 senses host VBUS through R403 820ohm. */
    hpcd_USB_OTG_HS.Init.use_external_vbus = DISABLE;
    HAL_StatusTypeDef s = HAL_PCD_Init(&hpcd_USB_OTG_HS);
    if (s != HAL_OK) return status(s);
    /* FIFO words, total 256 of the H723 HS core's 1024. Each Tx FIFO is
     * at least 16 words; Rx comfortably contains EP0 and vendor OUT. */
    if (HAL_PCDEx_SetRxFiFo(&hpcd_USB_OTG_HS,128) != HAL_OK ||
        HAL_PCDEx_SetTxFiFo(&hpcd_USB_OTG_HS,0,32) != HAL_OK ||
        HAL_PCDEx_SetTxFiFo(&hpcd_USB_OTG_HS,1,16) != HAL_OK ||
        HAL_PCDEx_SetTxFiFo(&hpcd_USB_OTG_HS,2,16) != HAL_OK ||
        HAL_PCDEx_SetTxFiFo(&hpcd_USB_OTG_HS,3,16) != HAL_OK ||
        HAL_PCDEx_SetTxFiFo(&hpcd_USB_OTG_HS,4,32) != HAL_OK) return USBD_FAIL;
    return USBD_OK;
}
USBD_StatusTypeDef USBD_LL_DeInit(USBD_HandleTypeDef *d) { return status(HAL_PCD_DeInit(d->pData)); }
USBD_StatusTypeDef USBD_LL_Start(USBD_HandleTypeDef *d) { return status(HAL_PCD_Start(d->pData)); }
USBD_StatusTypeDef USBD_LL_Stop(USBD_HandleTypeDef *d) { return status(HAL_PCD_Stop(d->pData)); }
USBD_StatusTypeDef USBD_LL_OpenEP(USBD_HandleTypeDef *d,uint8_t ep,uint8_t type,uint16_t size)
{ return status(HAL_PCD_EP_Open(d->pData,ep,size,type)); }
USBD_StatusTypeDef USBD_LL_CloseEP(USBD_HandleTypeDef *d,uint8_t ep)
{ return status(HAL_PCD_EP_Close(d->pData,ep)); }
USBD_StatusTypeDef USBD_LL_FlushEP(USBD_HandleTypeDef *d,uint8_t ep)
{
    if ((ep & 0x80U) && (ep & 0x7FU) >= 1U && (ep & 0x7FU) <= 4U)
        return status(quiesce_in(d->pData, ep & 0x7FU));
    return status(HAL_PCD_EP_Flush(d->pData,ep));
}
USBD_StatusTypeDef USBD_LL_StallEP(USBD_HandleTypeDef *d,uint8_t ep)
{ return status(HAL_PCD_EP_SetStall(d->pData,ep)); }
USBD_StatusTypeDef USBD_LL_ClearStallEP(USBD_HandleTypeDef *d,uint8_t ep)
{ return status(HAL_PCD_EP_ClrStall(d->pData,ep)); }
uint8_t USBD_LL_IsStallEP(USBD_HandleTypeDef *d,uint8_t ep)
{
    PCD_HandleTypeDef *h=d->pData;
    return (ep & 0x80U) ? h->IN_ep[ep&0x7FU].is_stall : h->OUT_ep[ep&0x7FU].is_stall;
}
USBD_StatusTypeDef USBD_LL_SetUSBAddress(USBD_HandleTypeDef *d,uint8_t address)
{ return status(HAL_PCD_SetAddress(d->pData,address)); }
USBD_StatusTypeDef USBD_LL_Transmit(USBD_HandleTypeDef *d,uint8_t ep,uint8_t *data,uint32_t size)
{ return status(HAL_PCD_EP_Transmit(d->pData,ep,data,size)); }
USBD_StatusTypeDef USBD_LL_PrepareReceive(USBD_HandleTypeDef *d,uint8_t ep,uint8_t *data,uint32_t size)
{ return status(HAL_PCD_EP_Receive(d->pData,ep,data,size)); }
uint32_t USBD_LL_GetRxDataSize(USBD_HandleTypeDef *d,uint8_t ep)
{ return HAL_PCD_EP_GetRxCount(d->pData,ep); }
void USBD_LL_Delay(uint32_t ms) { HAL_Delay(ms); }
/* This class owns static buffers and never allocates. */
void *USBD_static_malloc(uint32_t size) { (void)size; return NULL; }
void USBD_static_free(void *p) { (void)p; }
