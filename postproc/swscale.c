
// Software scaling and colorspace conversion routines for MPlayer

#include <inttypes.h>
#include "../config.h"

#undef HAVE_MMX2 //code is buggy
//#undef HAVE_MMX

#define RET 0xC3 //near return opcode

// temporary storage for 4 yuv lines:
// 16bit for now (mmx likes it more compact)
static uint16_t pix_buf_y[4][2048];
static uint16_t pix_buf_uv[2][2048*2];

// clipping helper table for C implementations:
static unsigned char clip_table[768];

// yuv->rgb conversion tables:
static    int yuvtab_2568[256];
static    int yuvtab_3343[256];
static    int yuvtab_0c92[256];
static    int yuvtab_1a1e[256];
static    int yuvtab_40cf[256];

static uint64_t yCoeff=    0x2568256825682568LL;
static uint64_t ubCoeff=   0x3343334333433343LL;
static uint64_t vrCoeff=   0x40cf40cf40cf40cfLL;
static uint64_t ugCoeff=   0xE5E2E5E2E5E2E5E2LL;
static uint64_t vgCoeff=   0xF36EF36EF36EF36ELL;
static uint64_t w80=       0x0080008000800080LL;
static uint64_t w10=       0x0010001000100010LL;

static uint64_t b16Dither= 0x0004000400040004LL;
static uint64_t b16Dither1=0x0004000400040004LL;
static uint64_t b16Dither2=0x0602060206020602LL;
static uint64_t g16Dither= 0x0002000200020002LL;
static uint64_t g16Dither1=0x0002000200020002LL;
static uint64_t g16Dither2=0x0301030103010301LL;

static uint64_t b16Mask=   0x001F001F001F001FLL;
static uint64_t g16Mask=   0x07E007E007E007E0LL;
static uint64_t r16Mask=   0xF800F800F800F800LL;
static uint64_t temp0;

static uint8_t funnyYCode[10000];
static uint8_t funnyUVCode[10000];



// *** bilinear scaling and yuv->rgb conversion of yv12 slices:
// *** Note: it's called multiple times while decoding a frame, first time y==0
// *** Designed to upscale, but may work for downscale too.
// s_xinc = (src_width << 8) / dst_width
// s_yinc = (src_height << 16) / dst_height
void SwScale_YV12slice_brg24(unsigned char* srcptr[],int stride[], int y, int h,
			     unsigned char* dstptr, int dststride, int dstw, int dstbpp,
			     unsigned int s_xinc,unsigned int s_yinc){

// scaling factors:
//static int s_yinc=(vo_dga_src_height<<16)/vo_dga_vp_height;
//static int s_xinc=(vo_dga_src_width<<8)/vo_dga_vp_width;

unsigned int s_xinc2=s_xinc>>1;

static int s_srcypos;
static int s_ypos;
static int s_last_ypos;
static int static_dstw;

#ifdef HAVE_MMX2
static int old_dstw= -1;
static int old_s_xinc= -1;
#endif

s_xinc&= -2; //clear last bit or uv and y might be shifted relative to each other

  if(y==0){
      s_srcypos=-2*s_yinc;
      s_ypos=-2;
      s_last_ypos=-2;
#ifdef HAVE_MMX2
// cant downscale !!!
	if(old_s_xinc != s_xinc || old_dstw!=dstw)
	{
		uint8_t *fragment;
		int imm8OfPShufW1;
		int imm8OfPShufW2;
		int fragmentLength;

		int xpos, xx, xalpha, i;

		old_s_xinc= s_xinc;
		old_dstw= dstw;

		static_dstw= dstw;

		// create an optimized horizontal scaling routine

		//code fragment

//		fragmentLength=0;
//		printf("%d, %d\n", fragmentLength,imm8OfPShufW1);

		asm volatile(
			"jmp 9f				\n\t"
		// Begin
			"0:				\n\t"
			"movq (%%esi, %%ebx), %%mm0	\n\t" //FIXME Alignment
			"movq %%mm0, %%mm1		\n\t"
			"psrlq $8, %%mm0		\n\t"
			"punpcklbw %%mm7, %%mm1	\n\t"
			"punpcklbw %%mm7, %%mm0	\n\t"
			"pshufw $0xFF, %%mm1, %%mm1	\n\t"
			"1:				\n\t"
			"pshufw $0xFF, %%mm0, %%mm0	\n\t"
			"2:				\n\t"
			"psubw %%mm1, %%mm0		\n\t"
			"psraw $1, %%mm0		\n\t"
			"pmullw %%mm2, %%mm0		\n\t"
			"psllw $7, %%mm1		\n\t"
			"paddw %%mm1, %%mm0		\n\t"
			"movq %%mm0, (%%edi, %%eax)	\n\t"
			"paddb %%mm6, %%mm2		\n\t" // 2*alpha += xpos&0xFF

			"addb %%ch, %%cl		\n\t" //2*xalpha += (4*s_xinc)&0xFF
			"adcl %%edx, %%ebx		\n\t" //xx+= (4*s_xinc)>>8 + carry

			"addl $8, %%eax			\n\t"
		// End
			"9:				\n\t"
//		"int $3\n\t"
			"leal 0b, %0			\n\t"
			"leal 1b, %1			\n\t"
			"leal 2b, %2			\n\t"
			"decl %1			\n\t"
			"decl %2			\n\t"
			"subl %0, %1			\n\t"
			"subl %0, %2			\n\t"
			"leal 9b, %3			\n\t"
			"subl %0, %3			\n\t"
			:"=r" (fragment), "=r" (imm8OfPShufW1), "=r" (imm8OfPShufW2),
			 "=r" (fragmentLength)
		);

		xpos= xx=xalpha= 0;
		//FIXME choose size and or xinc so that they fit exactly
		for(i=0; i<dstw/8; i++)
		{
			int xx=xpos>>8;

			if((i&3) == 0)
			{
				int a=0;
				int b=((xpos+s_xinc)>>8) - xx;
				int c=((xpos+s_xinc*2)>>8) - xx;
				int d=((xpos+s_xinc*3)>>8) - xx;

				memcpy(funnyYCode + fragmentLength*i/4, fragment, fragmentLength);

				funnyYCode[fragmentLength*i/4 + imm8OfPShufW1]=
				funnyYCode[fragmentLength*i/4 + imm8OfPShufW2]=
					a | (b<<2) | (c<<4) | (d<<6);

				funnyYCode[fragmentLength*(i+4)/4]= RET;
			}
			xpos+=s_xinc;
		}

		xpos= xx=xalpha= 0;
		//FIXME choose size and or xinc so that they fit exactly
		for(i=0; i<dstw/8; i++)
		{
			int xx=xpos>>8;

			if((i&3) == 0)
			{
				int a=0;
				int b=((xpos+s_xinc2)>>8) - xx;
				int c=((xpos+s_xinc2*2)>>8) - xx;
				int d=((xpos+s_xinc2*3)>>8) - xx;

				memcpy(funnyUVCode + fragmentLength*i/4, fragment, fragmentLength);

				funnyUVCode[fragmentLength*i/4 + imm8OfPShufW1]=
				funnyUVCode[fragmentLength*i/4 + imm8OfPShufW2]=
					a | (b<<2) | (c<<4) | (d<<6);

				funnyUVCode[fragmentLength*(i+4)/4]= RET;
			}
			xpos+=s_xinc2;
		}
//		funnyCode[0]= RET;


	}
#endif
  } // reset counters

  while(1){
    unsigned char *dest=dstptr+dststride*s_ypos;
    int y0=2+(s_srcypos>>16);
    int y1=1+(s_srcypos>>17);
    int yalpha=(s_srcypos&0xFFFF)>>7;
    int yalpha1=yalpha^511;
    int uvalpha=((s_srcypos>>1)&0xFFFF)>>7;
    int uvalpha1=uvalpha^511;
    uint16_t *buf0=pix_buf_y[y0&3];
    uint16_t *buf1=pix_buf_y[((y0+1)&3)];
    uint16_t *uvbuf0=pix_buf_uv[y1&1];
    uint16_t *uvbuf1=pix_buf_uv[(y1&1)^1];
    int i;

    if(y0>=y+h) break;

    s_ypos++; s_srcypos+=s_yinc;

    if(s_last_ypos!=y0){
      unsigned char *src=srcptr[0]+(y0-y)*stride[0];
      unsigned int xpos=0;
      s_last_ypos=y0;
      // *** horizontal scale Y line to temp buffer
      // this loop should be rewritten in MMX assembly!!!!
#ifdef HAVE_MMX2
	asm volatile(
		"pxor %%mm7, %%mm7		\n\t"
		"pxor %%mm2, %%mm2		\n\t" // 2*xalpha
		"movd %5, %%mm6			\n\t" // s_xinc&0xFF
		"punpcklwd %%mm6, %%mm6		\n\t"
		"punpcklwd %%mm6, %%mm6		\n\t"
		"movq %%mm6, %%mm2		\n\t"
		"psllq $16, %%mm2		\n\t"
		"paddb %%mm6, %%mm2		\n\t"
		"psllq $16, %%mm2		\n\t"
		"paddb %%mm6, %%mm2		\n\t"
		"psllq $16, %%mm2		\n\t" //0,t,2t,3t		t=s_xinc&0xFF
		"movq %%mm2, temp0		\n\t"
		"movd %4, %%mm6			\n\t" //(s_xinc*4)&0xFF
		"punpcklwd %%mm6, %%mm6		\n\t"
		"punpcklwd %%mm6, %%mm6		\n\t"
		"xorl %%eax, %%eax		\n\t" // i
		"xorl %%ebx, %%ebx		\n\t" // xx
		"movl %0, %%esi			\n\t" // src
		"movl %1, %%edi			\n\t" // buf1
		"movl %3, %%edx			\n\t" // (s_xinc*4)>>8
		"xorl %%ecx, %%ecx		\n\t"
		"movb %4, %%ch			\n\t" // (s_xinc*4)&0xFF
//	"int $3\n\t"
		"call funnyYCode			\n\t"
		"movq temp0, %%mm2		\n\t"
		"xorb %%cl, %%cl		\n\t"
		"call funnyYCode			\n\t"
		"movq temp0, %%mm2		\n\t"
		"xorb %%cl, %%cl		\n\t"
		"call funnyYCode			\n\t"
		"movq temp0, %%mm2		\n\t"
		"xorb %%cl, %%cl		\n\t"
		"call funnyYCode			\n\t"
		"movq temp0, %%mm2		\n\t"
		"xorb %%cl, %%cl		\n\t"
		"call funnyYCode			\n\t"
		"movq temp0, %%mm2		\n\t"
		"xorb %%cl, %%cl		\n\t"
		"call funnyYCode			\n\t"
		"movq temp0, %%mm2		\n\t"
		"xorb %%cl, %%cl		\n\t"
		"call funnyYCode			\n\t"
		"movq temp0, %%mm2		\n\t"
		"xorb %%cl, %%cl		\n\t"
		"call funnyYCode			\n\t"
		:: "m" (src), "m" (buf1), "m" (dstw), "m" ((s_xinc*4)>>8),
		  "m" ((s_xinc*4)&0xFF), "m" (s_xinc&0xFF)
		: "%eax", "%ebx", "%ecx", "%edx", "%esi", "%edi"
	);

#elif defined (ARCH_X86)
	//NO MMX just normal asm ... FIXME try/write funny MMX2 variant
	//FIXME add prefetch
	asm volatile(
		"xorl %%eax, %%eax		\n\t" // i
		"xorl %%ebx, %%ebx		\n\t" // xx
		"xorl %%ecx, %%ecx		\n\t" // 2*xalpha
		"1:				\n\t"
		"movzbl  (%0, %%ebx), %%edi	\n\t" //src[xx]
		"movzbl 1(%0, %%ebx), %%esi	\n\t" //src[xx+1]
		"subl %%edi, %%esi		\n\t" //src[xx+1] - src[xx]
		"imull %%ecx, %%esi		\n\t" //(src[xx+1] - src[xx])*2*xalpha
		"shll $8, %%edi			\n\t"
		"addl %%edi, %%esi		\n\t" //src[xx+1]*2*xalpha + src[xx]*(1-2*xalpha)
		"movl %1, %%edi			\n\t"
		"shrl $1, %%esi			\n\t"
		"movw %%si, (%%edi, %%eax, 2)	\n\t"
		"addb %4, %%cl			\n\t" //2*xalpha += s_xinc&0xFF
		"adcl %3, %%ebx			\n\t" //xx+= s_xinc>>8 + carry

		"movzbl (%0, %%ebx), %%edi	\n\t" //src[xx]
		"movzbl 1(%0, %%ebx), %%esi	\n\t" //src[xx+1]
		"subl %%edi, %%esi		\n\t" //src[xx+1] - src[xx]
		"imull %%ecx, %%esi		\n\t" //(src[xx+1] - src[xx])*2*xalpha
		"shll $8, %%edi			\n\t"
		"addl %%edi, %%esi		\n\t" //src[xx+1]*2*xalpha + src[xx]*(1-2*xalpha)
		"movl %1, %%edi			\n\t"
		"shrl $1, %%esi			\n\t"
		"movw %%si, 2(%%edi, %%eax, 2)	\n\t"
		"addb %4, %%cl			\n\t" //2*xalpha += s_xinc&0xFF
		"adcl %3, %%ebx			\n\t" //xx+= s_xinc>>8 + carry


		"addl $2, %%eax			\n\t"
		"cmpl %2, %%eax			\n\t"
		" jb 1b				\n\t"


		:: "r" (src), "m" (buf1), "m" (dstw), "m" (s_xinc>>8), "m" (s_xinc&0xFF)
		: "%eax", "%ebx", "%ecx", "%edi", "%esi"
		);
#else
      for(i=0;i<dstw;i++){
	register unsigned int xx=xpos>>8;
        register unsigned int xalpha=(xpos&0xFF)>>1;
	buf1[i]=(src[xx]*(xalpha^127)+src[xx+1]*xalpha);
	xpos+=s_xinc;
      }
#endif
      // *** horizontal scale U and V lines to temp buffer
      if(!(y0&1)){
        unsigned char *src1=srcptr[1]+(y1-y/2)*stride[1];
        unsigned char *src2=srcptr[2]+(y1-y/2)*stride[2];
        xpos=0;
        // this loop should be rewritten in MMX assembly!!!!
#ifdef HAVE_MMX2
	asm volatile(
		"pxor %%mm7, %%mm7		\n\t"
		"pxor %%mm2, %%mm2		\n\t" // 2*xalpha
		"movd %5, %%mm6			\n\t" // s_xinc&0xFF
		"punpcklwd %%mm6, %%mm6		\n\t"
		"punpcklwd %%mm6, %%mm6		\n\t"
		"movq %%mm6, %%mm2		\n\t"
		"psllq $16, %%mm2		\n\t"
		"paddb %%mm6, %%mm2		\n\t"
		"psllq $16, %%mm2		\n\t"
		"paddb %%mm6, %%mm2		\n\t"
		"psllq $16, %%mm2		\n\t" //0,t,2t,3t		t=s_xinc&0xFF
		"movq %%mm2, temp0		\n\t"
		"movd %4, %%mm6			\n\t" //(s_xinc*4)&0xFF
		"punpcklwd %%mm6, %%mm6		\n\t"
		"punpcklwd %%mm6, %%mm6		\n\t"
		"xorl %%eax, %%eax		\n\t" // i
		"xorl %%ebx, %%ebx		\n\t" // xx
		"movl %0, %%esi			\n\t" // src
		"movl %1, %%edi			\n\t" // buf1
		"movl %3, %%edx			\n\t" // (s_xinc*4)>>8
		"xorl %%ecx, %%ecx		\n\t"
		"movb %4, %%ch			\n\t" // (s_xinc*4)&0xFF
//	"int $3\n\t"
		"call funnyUVCode			\n\t"
		"movq temp0, %%mm2		\n\t"
		"xorb %%cl, %%cl		\n\t"
		"call funnyUVCode			\n\t"
		"movq temp0, %%mm2		\n\t"
		"xorb %%cl, %%cl		\n\t"
		"call funnyUVCode			\n\t"
		"movq temp0, %%mm2		\n\t"
		"xorb %%cl, %%cl		\n\t"
		"call funnyUVCode			\n\t"
		"movq temp0, %%mm2		\n\t"
		"xorb %%cl, %%cl		\n\t"
		"call funnyUVCode			\n\t"
		"movq temp0, %%mm2		\n\t"
		"xorb %%cl, %%cl		\n\t"
		"call funnyUVCode			\n\t"
		"movq temp0, %%mm2		\n\t"
		"xorb %%cl, %%cl		\n\t"
		"call funnyUVCode			\n\t"
		"movq temp0, %%mm2		\n\t"
		"xorb %%cl, %%cl		\n\t"
		"call funnyUVCode			\n\t"

		"xorl %%eax, %%eax		\n\t" // i
		"xorl %%ebx, %%ebx		\n\t" // xx
		"movl %6, %%esi			\n\t" // src
		"movl %1, %%edi			\n\t" // buf1
		"addl $4096, %%edi		\n\t"

		"call funnyUVCode			\n\t"
		"movq temp0, %%mm2		\n\t"
		"xorb %%cl, %%cl		\n\t"
		"call funnyUVCode			\n\t"
		"movq temp0, %%mm2		\n\t"
		"xorb %%cl, %%cl		\n\t"
		"call funnyUVCode			\n\t"
		"movq temp0, %%mm2		\n\t"
		"xorb %%cl, %%cl		\n\t"
		"call funnyUVCode			\n\t"
		"movq temp0, %%mm2		\n\t"
		"xorb %%cl, %%cl		\n\t"
		"call funnyUVCode			\n\t"
		"movq temp0, %%mm2		\n\t"
		"xorb %%cl, %%cl		\n\t"
		"call funnyUVCode			\n\t"
		"movq temp0, %%mm2		\n\t"
		"xorb %%cl, %%cl		\n\t"
		"call funnyUVCode			\n\t"
		"movq temp0, %%mm2		\n\t"
		"xorb %%cl, %%cl		\n\t"
		"call funnyUVCode			\n\t"

		:: "m" (src1), "m" (uvbuf1), "m" (dstw), "m" ((s_xinc2*4)>>8),
		  "m" ((s_xinc2*4)&0xFF), "m" (s_xinc2&0xFF), "m" (src2)
		: "%eax", "%ebx", "%ecx", "%edx", "%esi", "%edi"
	);

#elif defined (ARCH_X86)
	//NO MMX just normal asm ... FIXME try/write funny MMX2 variant
	asm volatile(
		"xorl %%eax, %%eax		\n\t" // i
		"xorl %%ebx, %%ebx		\n\t" // xx
		"xorl %%ecx, %%ecx		\n\t" // 2*xalpha
		"1:				\n\t"
		"movl %0, %%esi			\n\t"
		"movzbl  (%%esi, %%ebx), %%edi	\n\t" //src[xx]
		"movzbl 1(%%esi, %%ebx), %%esi	\n\t" //src[xx+1]
		"subl %%edi, %%esi		\n\t" //src[xx+1] - src[xx]
		"imull %%ecx, %%esi		\n\t" //(src[xx+1] - src[xx])*2*xalpha
		"shll $8, %%edi			\n\t"
		"addl %%edi, %%esi		\n\t" //src[xx+1]*2*xalpha + src[xx]*(1-2*xalpha)
		"movl %1, %%edi			\n\t"
		"shrl $1, %%esi			\n\t"
		"movw %%si, (%%edi, %%eax, 2)	\n\t"

		"movzbl  (%5, %%ebx), %%edi	\n\t" //src[xx]
		"movzbl 1(%5, %%ebx), %%esi	\n\t" //src[xx+1]
		"subl %%edi, %%esi		\n\t" //src[xx+1] - src[xx]
		"imull %%ecx, %%esi		\n\t" //(src[xx+1] - src[xx])*2*xalpha
		"shll $8, %%edi			\n\t"
		"addl %%edi, %%esi		\n\t" //src[xx+1]*2*xalpha + src[xx]*(1-2*xalpha)
		"movl %1, %%edi			\n\t"
		"shrl $1, %%esi			\n\t"
		"movw %%si, 4096(%%edi, %%eax, 2)\n\t"

		"addb %4, %%cl			\n\t" //2*xalpha += s_xinc&0xFF
		"adcl %3, %%ebx			\n\t" //xx+= s_xinc>>8 + carry
		"addl $1, %%eax			\n\t"
		"cmpl %2, %%eax			\n\t"
		" jb 1b				\n\t"


		:: "m" (src1), "m" (uvbuf1), "m" (dstw), "m" (s_xinc2>>8), "m" (s_xinc2&0xFF),
		"r" (src2)
		: "%eax", "%ebx", "%ecx", "%edi", "%esi"
		);
#else
        for(i=0;i<dstw;i++){
	  register unsigned int xx=xpos>>8;
          register unsigned int xalpha=(xpos&0xFF)>>1;
	  uvbuf1[i]=(src1[xx]*(xalpha^127)+src1[xx+1]*xalpha);
	  uvbuf1[i+2048]=(src2[xx]*(xalpha^127)+src2[xx+1]*xalpha);
	  xpos+=s_xinc2;
        }
#endif
      }
      if(!y0) continue;
    }

    // Note1: this code can be resticted to n*8 (or n*16) width lines to simplify optimization...
    // Re: Note1: ok n*4 for now
    // Note2: instead of using lookup tabs, mmx version could do the multiply...
    // Re: Note2: yep
    // Note3: maybe we should make separated 15/16, 24 and 32bpp version of this:
    // Re: done (32 & 16) and 16 has dithering :) but 16 is untested
#ifdef HAVE_MMX
	//FIXME write lq version with less uv ...
	//FIXME reorder / optimize
	if(dstbpp == 32)
	{
		asm volatile(

#define YSCALEYUV2RGB \
		"pxor %%mm7, %%mm7		\n\t"\
		"movd %6, %%mm6			\n\t" /*yalpha1*/\
		"punpcklwd %%mm6, %%mm6		\n\t"\
		"punpcklwd %%mm6, %%mm6		\n\t"\
		"movd %7, %%mm5			\n\t" /*uvalpha1*/\
		"punpcklwd %%mm5, %%mm5		\n\t"\
		"punpcklwd %%mm5, %%mm5		\n\t"\
		"xorl %%eax, %%eax		\n\t"\
		"1:				\n\t"\
		"movq (%0, %%eax, 2), %%mm0	\n\t" /*buf0[eax]*/\
		"movq (%1, %%eax, 2), %%mm1	\n\t" /*buf1[eax]*/\
		"psubw %%mm1, %%mm0		\n\t" /* buf0[eax] - buf1[eax]*/\
		"pmulhw %%mm6, %%mm0		\n\t" /* (buf0[eax] - buf1[eax])yalpha1>>16*/\
		"psraw $7, %%mm1		\n\t" /* buf0[eax] - buf1[eax] >>7*/\
		"paddw %%mm0, %%mm1		\n\t" /* buf0[eax]yalpha1 + buf1[eax](1-yalpha1) >>16*/\
		"psubw w10, %%mm1		\n\t" /* Y-16*/\
		"psllw $3, %%mm1		\n\t" /* (y-16)*8*/\
		"pmulhw yCoeff, %%mm1		\n\t"\
\
		"movq (%2, %%eax,2), %%mm2	\n\t" /* uvbuf0[eax]*/\
		"movq (%3, %%eax,2), %%mm3	\n\t" /* uvbuf1[eax]*/\
		"psubw %%mm3, %%mm2		\n\t" /* uvbuf0[eax] - uvbuf1[eax]*/\
		"pmulhw %%mm5, %%mm2		\n\t" /* (uvbuf0[eax] - uvbuf1[eax])uvalpha1>>16*/\
		"psraw $7, %%mm3		\n\t" /* uvbuf0[eax] - uvbuf1[eax] >>7*/\
		"paddw %%mm2, %%mm3		\n\t" /* uvbuf0[eax]uvalpha1 - uvbuf1[eax](1-uvalpha1)*/\
		"psubw w80, %%mm3		\n\t" /* (U-128)*/\
		"psllw $3, %%mm3		\n\t" /*(U-128)8*/\
\
		"movq 4096(%2, %%eax,2), %%mm4	\n\t" /* uvbuf0[eax+2048]*/\
		"movq 4096(%3, %%eax,2), %%mm0	\n\t" /* uvbuf1[eax+2048]*/\
		"psubw %%mm0, %%mm4		\n\t" /* uvbuf0[eax+2048] - uvbuf1[eax+2048]*/\
		"pmulhw %%mm5, %%mm4		\n\t" /* (uvbuf0[eax+2048] - uvbuf1[eax+2048])uvalpha1>>16*/\
		"psraw $7, %%mm0		\n\t" /* uvbuf0[eax+2048] - uvbuf1[eax+2048] >>7*/\
		"paddw %%mm4, %%mm0		\n\t" /* uvbuf0[eax+2048]uvalpha1 - uvbuf1[eax+2048](1-uvalpha1)*/\
		"psubw w80, %%mm0		\n\t" /* (V-128)*/\
		"psllw $3, %%mm0		\n\t" /* (V-128)8*/\
\
		"movq %%mm3, %%mm2		\n\t" /* (U-128)8*/\
		"pmulhw ubCoeff, %%mm3		\n\t"\
		"paddw %%mm1, %%mm3		\n\t" /* B*/\
\
		"movq %%mm0, %%mm4		\n\t" /* (V-128)8*/\
		"pmulhw vrCoeff, %%mm0		\n\t"\
		"paddw %%mm1, %%mm0		\n\t" /* R*/\
\
		"pmulhw ugCoeff, %%mm2		\n\t"\
		"pmulhw vgCoeff, %%mm4		\n\t"\
		"paddw %%mm4, %%mm2		\n\t"\
		"paddw %%mm2, %%mm1		\n\t" /* G*/\
\
		"packuswb %%mm3, %%mm3		\n\t"\
		"packuswb %%mm0, %%mm0		\n\t"\
		"packuswb %%mm1, %%mm1		\n\t"

YSCALEYUV2RGB
		"punpcklbw %%mm1, %%mm3		\n\t" // BGBGBGBG
		"punpcklbw %%mm7, %%mm0		\n\t" // R0R0R0R0

		"movq %%mm3, %%mm1		\n\t"
		"punpcklwd %%mm0, %%mm3		\n\t" // BGR0BGR0
		"punpckhwd %%mm0, %%mm1		\n\t" // BGR0BGR0
#ifdef HAVE_MMX2
		"movntq %%mm3, (%4, %%eax, 4)	\n\t"
		"movntq %%mm1, 8(%4, %%eax, 4)	\n\t"
#else
		"movq %%mm3, (%4, %%eax, 4)	\n\t"
		"movq %%mm1, 8(%4, %%eax, 4)	\n\t"
#endif
		"addl $4, %%eax			\n\t"
		"cmpl %5, %%eax			\n\t"
		" jb 1b				\n\t"


		:: "r" (buf0), "r" (buf1), "r" (uvbuf0), "r" (uvbuf1), "r" (dest), "m" (dstw),
		"m" (yalpha1), "m" (uvalpha1)
		: "%eax"
		);
	}
	else if(dstbpp==16)
	{
		asm volatile(

YSCALEYUV2RGB
		"paddusb g16Dither, %%mm1	\n\t"
		"paddusb b16Dither, %%mm0	\n\t"
		"paddusb b16Dither, %%mm3	\n\t"
		"punpcklbw %%mm7, %%mm1		\n\t" // 0G0G0G0G
		"punpcklbw %%mm7, %%mm3		\n\t" // 0B0B0B0B
		"punpcklbw %%mm7, %%mm0		\n\t" // 0R0R0R0R

		"psrlw $3, %%mm3		\n\t"
		"psllw $3, %%mm1		\n\t"
		"psllw $8, %%mm0		\n\t"
		"pand g16Mask, %%mm1		\n\t"
		"pand r16Mask, %%mm0		\n\t"

		"por %%mm3, %%mm1		\n\t"
		"por %%mm1, %%mm0		\n\t"
#ifdef HAVE_MMX2
		"movntq %%mm0, (%4, %%eax, 2)	\n\t"
#else
		"movq %%mm0, (%4, %%eax, 2)	\n\t"
#endif
		"addl $4, %%eax			\n\t"
		"cmpl %5, %%eax			\n\t"
		" jb 1b				\n\t"

		:: "r" (buf0), "r" (buf1), "r" (uvbuf0), "r" (uvbuf1), "r" (dest), "m" (dstw),
		"m" (yalpha1), "m" (uvalpha1)
		: "%eax"
		);
	}
#else
	if(dstbpp==32 || dstbpp==24)
	{
		for(i=0;i<dstw;i++){
			// vertical linear interpolation && yuv2rgb in a single step:
			int Y=yuvtab_2568[((buf0[i]*yalpha1+buf1[i]*yalpha)>>16)];
			int U=((uvbuf0[i]*uvalpha1+uvbuf1[i]*uvalpha)>>16);
			int V=((uvbuf0[i+2048]*uvalpha1+uvbuf1[i+2048]*uvalpha)>>16);
			dest[0]=clip_table[((Y + yuvtab_3343[U]) >>13)];
			dest[1]=clip_table[((Y + yuvtab_0c92[V] + yuvtab_1a1e[U]) >>13)];
			dest[2]=clip_table[((Y + yuvtab_40cf[V]) >>13)];
			dest+=dstbpp>>3;
		}
	}
	else if(dstbpp==16) //16bit
	{
		for(i=0;i<dstw;i++){
			// vertical linear interpolation && yuv2rgb in a single step:
			int Y=yuvtab_2568[((buf0[i]*yalpha1+buf1[i]*yalpha)>>16)];
			int U=((uvbuf0[i]*uvalpha1+uvbuf1[i]*uvalpha)>>16);
			int V=((uvbuf0[i+2048]*uvalpha1+uvbuf1[i+2048]*uvalpha)>>16);

			((uint16_t*)dest)[0] =
				(clip_table[((Y + yuvtab_3343[U]) >>13)]>>3) |
				(clip_table[((Y + yuvtab_0c92[V] + yuvtab_1a1e[U]) >>13)]<<3)&0x07E0 |
				(clip_table[((Y + yuvtab_40cf[V]) >>13)]<<8)&0xF800;
			dest+=2;
		}
	}
	else if(dstbpp==15) //15bit FIXME how do i figure out if its 15 or 16?
	{
		for(i=0;i<dstw;i++){
			// vertical linear interpolation && yuv2rgb in a single step:
			int Y=yuvtab_2568[((buf0[i]*yalpha1+buf1[i]*yalpha)>>16)];
			int U=((uvbuf0[i]*uvalpha1+uvbuf1[i]*uvalpha)>>16);
			int V=((uvbuf0[i+2048]*uvalpha1+uvbuf1[i+2048]*uvalpha)>>16);

			((uint16_t*)dest)[0] =
				(clip_table[((Y + yuvtab_3343[U]) >>13)]>>3) |
				(clip_table[((Y + yuvtab_0c92[V] + yuvtab_1a1e[U]) >>13)]<<2)&0x03E0 |
				(clip_table[((Y + yuvtab_40cf[V]) >>13)]<<7)&0x7C00;
			dest+=2;
		}
	}
#endif

	b16Dither= b16Dither1;
	b16Dither1= b16Dither2;
	b16Dither2= b16Dither;

	g16Dither= g16Dither1;
	g16Dither1= g16Dither2;
	g16Dither2= g16Dither;
  }

}


void SwScale_Init(){
    // generating tables:
    int i;
    for(i=0;i<256;i++){
        clip_table[i]=0;
        clip_table[i+256]=i;
        clip_table[i+512]=255;
	yuvtab_2568[i]=(0x2568*(i-16))+(256<<13);
	yuvtab_3343[i]=0x3343*(i-128);
	yuvtab_0c92[i]=-0x0c92*(i-128);
	yuvtab_1a1e[i]=-0x1a1e*(i-128);
	yuvtab_40cf[i]=0x40cf*(i-128);
    }

}
