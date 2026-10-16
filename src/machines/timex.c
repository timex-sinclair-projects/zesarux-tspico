/*
    ZEsarUX  ZX Second-Emulator And Released for UniX
    Copyright (C) 2013 Cesar Hernandez Bano

    This file is part of ZEsarUX.

    ZEsarUX is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#include <stdio.h>
#include <stdlib.h>

#include "cpu.h"
#include "timex.h"
#include "debug.h"
#include "contend.h"
#include "mem128.h"
#include "zxvision.h"
#include "tape.h"

#include "chloe.h"
#include "prism.h"
#include "zxuno.h"
#include "tbblue.h"
#include "settings.h"



z80_byte timex_port_f4;
//Para Timex Video modes
z80_bit timex_video_emulation={0};
z80_byte timex_port_ff=0;




//Para poder hacer 512x192 en una parte de la pantalla solamente
int timex_ugly_hack_enabled=0;
int timex_ugly_hack_last_hires=0; //a partir de que coordenada se hace cambio



//Direcciones donde estan cada pagina de rom. 1 pagina de 16 kb
z80_byte *timex_rom_mem_table[1];

//Direcciones donde estan cada pagina de ram home. 3 paginas de 16 kb cada una
z80_byte *timex_home_ram_mem_table[3];

//Direcciones donde estan cada pagina de ram ex
z80_byte *timex_ex_rom_mem_table[8];

//Direcciones donde estan cada pagina de ram dock
z80_byte *timex_dock_rom_mem_table[8];

//Direcciones actuales mapeadas, bloques de 8 kb
z80_byte *timex_memory_paged[8];


//Si real, es 512x192 sin escalado, pero no permite efectos de linea
//Si no, es 512x192 escalado a 256x192 y permite efectos (cambios de modo) en el mismo frame
z80_bit timex_mode_512192_real={1};


//Tipos de memoria mapeadas
//0=rom
//1=home
//2=dock
//3=ex
//Constantes definidas en TIMEX_MEMORY_TYPE_ROM, _HOME, _DOCK, _EX
z80_byte timex_type_memory_paged[8];


//Paginas mapeadas en cada zona de RAM. Se solamente usa en menu debug y breakpoints, no para el core de emulacion
z80_byte debug_timex_paginas_memoria_mapeadas[8];

z80_bit timex_cartridge_inserted={0};


//TS-2068 Megabank: 4 MB of battery-backed RAM as Timex expansion banks
//(github.com/timex-sinclair-projects/TS-2068-Megabank, docs/spec.md).
//Ports FDh (select a register) and FCh (read or write it), decoded from A7-A0.
//Each 8K chunk has an owner (OWN0-7, an expansion bank 1-253, 0 for none) and
//a sub-page (SUB0-7). A chunk owned by one of the board's banks reads and
//writes board RAM: the board pulls /BE, above HOME, EXROM and DOCK.
//Environment, read once:
//  MEGABANK_FILE=path  enables the board and keeps its RAM in that file (the
//                      battery): loaded at start, saved on exit
//  MEGABANK=1          enables the board without a file
//  MEGABANK_BASE       first bank: 1 (default), 65, 129 or 193
//  MEGABANK_SIZE       banks fitted: 8, 16, 32 or 64 (default)
//  MEGABANK_WP=1       the write-protect switch is on

#define MEGABANK_VERSION 1

static void megabank_reset_registers(void);

int megabank_enabled=0;
static int megabank_initialized=0;
static z80_byte *megabank_mem=NULL;
static char megabank_file[1024];
static z80_byte megabank_select;
static z80_byte megabank_own[8];
static z80_byte megabank_sub[8];
static z80_byte megabank_wprot;
static z80_byte megabank_base=1;
static z80_byte megabank_size=64;
static int megabank_wp_switch=0;

static int megabank_env_int(const char *name,int def)
{
	const char *s=getenv(name);
	if (s==NULL || *s==0) return def;
	return atoi(s);
}

void megabank_init(void)
{
	if (megabank_initialized) return;
	megabank_initialized=1;

	const char *file=getenv("MEGABANK_FILE");
	if (file!=NULL && *file) {
		snprintf(megabank_file,sizeof(megabank_file),"%s",file);
		megabank_enabled=1;
	}
	if (megabank_env_int("MEGABANK",0)) megabank_enabled=1;
	if (!megabank_enabled) return;

	int base=megabank_env_int("MEGABANK_BASE",1);
	if (base!=1 && base!=65 && base!=129 && base!=193) {
		debug_printf(VERBOSE_ERR,"MEGABANK_BASE must be 1, 65, 129 or 193, not %d; using 1",base);
		base=1;
	}
	int size=megabank_env_int("MEGABANK_SIZE",64);
	if (size!=8 && size!=16 && size!=32 && size!=64) {
		debug_printf(VERBOSE_ERR,"MEGABANK_SIZE must be 8, 16, 32 or 64, not %d; using 64",size);
		size=64;
	}
	megabank_base=base;
	megabank_size=size;
	megabank_wp_switch=megabank_env_int("MEGABANK_WP",0)!=0;

	megabank_mem=calloc(megabank_size,65536);
	if (megabank_mem==NULL) cpu_panic("Can not allocate memory for the Megabank");

	if (megabank_file[0]) {
		FILE *f=fopen(megabank_file,"rb");
		if (f!=NULL) {
			size_t n=fread(megabank_mem,1,(size_t)megabank_size*65536,f);
			fclose(f);
			debug_printf(VERBOSE_INFO,"Megabank: loaded %ld bytes from %s",(long)n,megabank_file);
		}
	}

	megabank_reset_registers();
	int last=megabank_base+megabank_size-1;
	debug_printf(VERBOSE_INFO,"Megabank: banks %d-%d%s",megabank_base,last>253?253:last,
		megabank_wp_switch?", write-protect switch on":"");
}

//The battery: keep the RAM for the next run
void megabank_save(void)
{
	if (!megabank_enabled || megabank_mem==NULL || !megabank_file[0]) return;
	FILE *f=fopen(megabank_file,"wb");
	if (f==NULL) {
		debug_printf(VERBOSE_ERR,"Megabank: can not write %s",megabank_file);
		return;
	}
	fwrite(megabank_mem,1,(size_t)megabank_size*65536,f);
	fclose(f);
}

//RESET: no chunk owned, SUBn = n, nothing write-protected. The RAM is kept.
static void megabank_reset_registers(void)
{
	int i;
	for (i=0;i<8;i++) {
		megabank_own[i]=0;
		megabank_sub[i]=i;
	}
	megabank_wprot=0;
	megabank_select=0;
}

void megabank_reset(void)
{
	if (!megabank_enabled) return;
	megabank_reset_registers();
	if (MACHINE_IS_TIMEX_TS_TC_2068) timex_set_memory_pages();
}

//Whether the board answers memory cycles in this chunk (pulls /BE)
static int megabank_owns(int chunk)
{
	if (!megabank_enabled) return 0;
	int owner=megabank_own[chunk];
	//FEh and FFh mean none: with BASE 193 only banks 193-253 are usable
	return owner>=megabank_base && owner<megabank_base+megabank_size && owner<=253;
}

int megabank_chunk_writable(int chunk)
{
	return !megabank_wp_switch && !(megabank_wprot & (1<<chunk));
}

int megabank_port(z80_byte port_l)
{
	return megabank_enabled && (port_l==0xFC || port_l==0xFD);
}

void megabank_out(z80_byte port_l,z80_byte value)
{
	if (port_l==0xFD) {
		megabank_select=value;
		return;
	}

	z80_byte reg=megabank_select;
	if (reg<0x08) {
		megabank_own[reg]=value;
		megabank_sub[reg]=reg;
	}
	else if (reg<0x10) megabank_sub[reg-8]=value&7;
	else if (reg==0x10) megabank_wprot=value;
	else return;   //read-only or unused

	timex_set_memory_pages();
}

z80_byte megabank_in(z80_byte port_l)
{
	if (port_l==0xFD) return megabank_select;

	z80_byte reg=megabank_select;
	if (reg<0x08) return megabank_own[reg];
	if (reg<0x10) return megabank_sub[reg-8];
	switch (reg) {
		case 0x10: return megabank_wprot;
		case 0x11: return megabank_wp_switch?1:0;   //bit 1, battery low: never here
		case 0x12: return 0xA5;
		case 0x13: return 0x5A;
		case 0x14: return megabank_base;
		case 0x15: return megabank_size;
		case 0x16: return MEGABANK_VERSION;
	}
	return 0xFF;
}


void timex_set_memory_pages(void)
{
	//timex_port_f4;
	//Cada bit determina si se mapea home(0) o dock/ex
	//timex_port_ff
	//Bit 7:    Selects which bank the horizontal MMU should use. 0=DOCK, 1=EX-ROM.

        z80_byte *puntero_rom=timex_rom_mem_table[0];

        timex_memory_paged[0]=puntero_rom;
        timex_memory_paged[1]=&puntero_rom[8192];

        timex_type_memory_paged[0]=TIMEX_MEMORY_TYPE_ROM;
        timex_type_memory_paged[1]=TIMEX_MEMORY_TYPE_ROM;

        debug_timex_paginas_memoria_mapeadas[0]=0;
        debug_timex_paginas_memoria_mapeadas[1]=0;


	//Por defecto suponemos mapeo:
	//16 kb rom, ram 0, ram 1, ram 2

	int pagina,bloque_8k;

	bloque_8k=2;

	for (pagina=0;pagina<3;pagina++) {

        	z80_byte *puntero_ram=timex_home_ram_mem_table[pagina];

	        timex_memory_paged[bloque_8k]=puntero_ram;
        	timex_type_memory_paged[bloque_8k]=TIMEX_MEMORY_TYPE_HOME;
	        debug_timex_paginas_memoria_mapeadas[bloque_8k]=pagina;
		bloque_8k++;

	        timex_memory_paged[bloque_8k]=&puntero_ram[8192];
        	timex_type_memory_paged[bloque_8k]=TIMEX_MEMORY_TYPE_HOME;
	        debug_timex_paginas_memoria_mapeadas[bloque_8k]=pagina;
		bloque_8k++;

	}


	//Aplicamos segun paginacion TIMEX 280SE
		int bloque_ram;
		z80_byte mascara_puerto_f4=1;

		for (bloque_ram=0;bloque_ram<8;bloque_ram++) {
			//Ver cada bit del puerto f4
			if (timex_port_f4 & mascara_puerto_f4) {
				//Se pagina ex o dock
			        //timex_port_ff
			        //Bit 7:    Selects which bank the horizontal MMU should use. 0=DOCK, 1=EX-ROM.
				z80_byte *puntero_memoria;
				if (timex_port_ff&128) {
					//EX
					puntero_memoria=timex_ex_rom_mem_table[bloque_ram];
					timex_type_memory_paged[bloque_ram]=TIMEX_MEMORY_TYPE_EX;

					//temp
					//printf ("forzamos menu\n");
					//menu_set_menu_abierto(1);


				}
				else {
					//DOCK
					puntero_memoria=timex_dock_rom_mem_table[bloque_ram];
					timex_type_memory_paged[bloque_ram]=TIMEX_MEMORY_TYPE_DOCK;
				}
				timex_memory_paged[bloque_ram]=puntero_memoria;
				debug_timex_paginas_memoria_mapeadas[bloque_ram]=bloque_ram;
			}

			mascara_puerto_f4=mascara_puerto_f4<<1;
		}

	//Megabank: an owned chunk is the board's, whatever F4h and FFh say
	for (bloque_ram=0;bloque_ram<8;bloque_ram++) {
		if (!megabank_owns(bloque_ram)) continue;
		int page=(megabank_own[bloque_ram]-megabank_base)*8+megabank_sub[bloque_ram];
		timex_memory_paged[bloque_ram]=&megabank_mem[page*8192];
		timex_type_memory_paged[bloque_ram]=TIMEX_MEMORY_TYPE_EXPANSION;
		debug_timex_paginas_memoria_mapeadas[bloque_ram]=megabank_own[bloque_ram];
	}

	//printf ("fin timex_set_memory_pages\n");

}


void timex_init_memory_tables(void)
{
	debug_printf (VERBOSE_DEBUG,"Initializing Timex memory pages");

	/*
//Direcciones donde estan cada pagina de rom. 2 paginas de 16 kb
//z80_byte *timex_rom_mem_table[2];

//Direcciones donde estan cada pagina de ram home
//z80_byte *timex_home_ram_mem_table[8];

//Direcciones donde estan cada pagina de ram ex
//z80_byte *timex_ex_rom_mem_table[8];

//Direcciones donde estan cada pagina de ram dock
//z80_byte *timex_dock_rom_mem_table[8];

//Direcciones actuales mapeadas, bloques de 8 kb
	*/

	//memoria_spectrum
	//16 kb rom
	//48 kb home
	//64 kb ex
	//64 kb dock

	megabank_init();

	z80_byte *puntero;
	puntero=memoria_spectrum;

	timex_rom_mem_table[0]=puntero;
	puntero +=16384;

	int i;
	for (i=0;i<3;i++) {
		timex_home_ram_mem_table[i]=puntero;
		puntero +=16384;
	}

	for (i=0;i<8;i++) {
		timex_ex_rom_mem_table[i]=puntero;
		puntero +=8192;
	}

	for (i=0;i<8;i++) {
		timex_dock_rom_mem_table[i]=puntero;
		puntero +=8192;
	}

	//Parece que los 8 kb de rom que se cargan en ex rom[0] tambien estan presentes en rom[1]
	//TS-Pico 16K-EXROM: keep EX segment 1 as its own 8 KB buffer so a 16 KB
	//EXROM (gus-exrom, code at $2000-$3FFF) maps real bytes. rom_load()
	//mirrors seg0 into seg1 for stock 8 KB EXROM images, preserving the
	//original mirror behavior for non-TS-Pico ROMs.
	//timex_ex_rom_mem_table[1]=timex_ex_rom_mem_table[0];
	//temp
	//timex_ex_rom_mem_table[2]=timex_ex_rom_mem_table[0];
	//timex_ex_rom_mem_table[3]=timex_ex_rom_mem_table[0];
	//timex_ex_rom_mem_table[4]=timex_ex_rom_mem_table[0];
	//timex_ex_rom_mem_table[5]=timex_ex_rom_mem_table[0];
	//timex_ex_rom_mem_table[6]=timex_ex_rom_mem_table[0];
	//timex_ex_rom_mem_table[7]=timex_ex_rom_mem_table[0];

	//prueba
	//timex_dock_rom_mem_table[0]=timex_ex_rom_mem_table[0];


}


//Inicializar cartucho con 255
void timex_empty_dock_space(void)
{

	debug_printf(VERBOSE_INFO,"Emptying timex dock memory");

	int i;

	z80_byte *puntero;

	puntero=timex_dock_rom_mem_table[0];

	for (i=0;i<65536;i++) {
		*puntero=255;
		puntero++;
	}

	timex_cartridge_inserted.v=0;
}


void timex_insert_dck_cartridge(char *filename)
{

	debug_printf(VERBOSE_INFO,"Inserting timex dock cartridge %s",filename);

        FILE *ptr_cartridge;
        ptr_cartridge=fopen(filename,"rb");

        if (!ptr_cartridge) {
		debug_printf (VERBOSE_ERR,"Unable to open cartridge file %s",filename);
                return;
        }

	//Leer primer byte identificador
	z80_byte dck_id;

        //int leidos=fread(mem,1,65535,ptr_cartridge);
        fread(&dck_id,1,1,ptr_cartridge);

	if (dck_id!=0) {
		debug_printf (VERBOSE_ERR,"DCK with id 0x%02X not supported",dck_id);
		return;
	}

	//flags para cada bloque de 8
	z80_byte block_flags[8];
	fread(block_flags,1,8,ptr_cartridge);

	//Leer cada bloque de 8 si conviene

	int bloque;

	for (bloque=0;bloque<8;bloque++) {
		if (block_flags[bloque]!=0) {
			if (block_flags[bloque]==2) {
				//Leer 8kb
				int segmento=bloque*8192;
				debug_printf (VERBOSE_DEBUG,"Loading 8kb block at Segment %04XH-%04XH",segmento,segmento+8191);
				fread(timex_dock_rom_mem_table[bloque],1,8192,ptr_cartridge);
			}

			else {
				debug_printf (VERBOSE_ERR,"8 KB block with id 0x%02X not supported",block_flags[bloque]);
				return;
			}
		}
	}


        fclose(ptr_cartridge);


        if (noautoload.v==0) {
                debug_printf (VERBOSE_INFO,"Reset cpu due to autoload");
                reset_cpu();
        }

	timex_cartridge_inserted.v=1;

}

/*
With the Timex hi res display, the border color is the same as the paper color in the second CLUT. Bits 3-5 of port FF set the ink, paper, and border values to the following ULAplus palette registers:
BITS INK PAPER BORDER
000 24 31 31 001 25 30 30
010 26 29 29
011 27 28 28
100 28 27 27
101 29 26 26
110 30 25 25
111 31 24 24

*/


//Retorna color tinta (sin brillo) para modo timex 6
int get_timex_ink_mode6_color(void)
{
        z80_byte col6=(timex_port_ff>>3)&7;
        return col6;
}


//Retorna color paper (sin brillo) para modo timex 6
int get_timex_paper_mode6_color(void)
{
        z80_byte col6=7-get_timex_ink_mode6_color();
        return col6;
}


//Retorna color border (con brillo)
int get_timex_border_mode6_color(void)
{

        return (7-get_timex_ink_mode6_color() )+8;
}



int timex_si_modo_512(void)
{
    //Si es modo timex 512x192, llamar a otra funcion
        if (MACHINE_IS_SPECTRUM) {
                z80_byte timex_video_mode=timex_port_ff&7;
                if (timex_video_emulation.v) {
                        if (timex_video_mode==4 || timex_video_mode==6) {
				return 1;
                        }
                }
        }

	return 0;
}



int timex_si_modo_512_y_zoom_par(void)
{
	if (timex_si_modo_512() ) {
		if ( (zoom_x&1)==0) return 1;
	}


	return 0;

}

int timex_si_modo_shadow(void)
{
     z80_byte timex_video_mode=timex_port_ff&7;

        if (timex_video_emulation.v) {
                //Modos de video Timex
                /*
000 - Video data at address 16384 and 8x8 color attributes at address 22528 (like on ordinary Spectrum);

001 - Video data at address 24576 and 8x8 color attributes at address 30720;

010 - Multicolor mode: video data at address 16384 and 8x1 color attributes at address 24576;

110 - Extended resolution: without color attributes, even columns of video data are taken from address 16384, and odd columns of video data are taken fr
om address 24576
                */

                if (timex_video_mode==1) return 1;

        }

        return 0;

}


int timex_si_modo_8x1(void)
{
     z80_byte timex_video_mode=timex_port_ff&7;

        if (timex_video_emulation.v) {
                //Modos de video Timex
                /*
000 - Video data at address 16384 and 8x8 color attributes at address 22528 (like on ordinary Spectrum);

001 - Video data at address 24576 and 8x8 color attributes at address 30720;

010 - Multicolor mode: video data at address 16384 and 8x1 color attributes at address 24576;

110 - Extended resolution: without color attributes, even columns of video data are taken from address 16384, and odd columns of video data are taken fr
om address 24576
                */

                if (timex_video_mode==2) return 1;

        }

        return 0;

}


void set_timex_port_ff(z80_byte value)
{
	if (timex_video_emulation.v) {

		if ( (timex_port_ff&7)!=(value&7)) {
	                char mensaje[200];

			if ((value&7)==0) sprintf (mensaje,"Setting Timex Video Mode 0 (standard screen 0)");
			else if ((value&7)==1) sprintf (mensaje,"Setting Timex Video Mode 1 (standard screen 1)");
			else if ((value&7)==2) sprintf (mensaje,"Setting Timex Video Mode 2 (hires colour 8x1)");
			else if ((value&7)==6) {
				if ( (zoom_x&1)==0 && timex_mode_512192_real.v) {
					sprintf (mensaje,"Setting Timex Video Mode 6 (512x192 monochrome)");
				}

				else if (MACHINE_IS_PRISM || MACHINE_IS_TBBLUE) {
					sprintf (mensaje,"Setting Timex Video Mode 6 (512x192 monochrome)");
                                }

				else {
					sprintf (mensaje,"Timex Video Mode 6 (512x192 monochrome) needs Timex Real 512x192 setting enabled and horizontal zoom even. Reducing to 256x192");
				}
			}
                        else sprintf (mensaje,"Setting Unknown Timex Video Mode %d",value);

                	screen_print_splash_text_center_no_if_previous(ESTILO_GUI_TINTA_NORMAL,ESTILO_GUI_PAPEL_NORMAL,mensaje);
		}

        	if ((value&7)==6) {
                        //Indicar que se ha puesto modo timex en alguna parte del frame
	                //timex_ugly_hack_last_hires=t_estados/screen_testados_linea;
        	        //printf ("estableciendo modo timex en y: %d\n",timex_ugly_hack_last_hires);
        	}


		z80_byte last_timex_port_ff=timex_port_ff;
		timex_port_ff=value;
		//Color del border en modo timex hi-res sale de aqui
		//Aunque con esto avisamos que el color del border en modo 512x192 se puede haber modificado
		modificado_border.v=1;
		if (last_timex_port_ff!=timex_port_ff) clear_putpixel_cache(); //porque se puede cambiar de modo, borrar la putpixel cache

		if (MACHINE_IS_CHLOE_280SE) chloe_set_memory_pages();
		if (MACHINE_IS_PRISM) prism_set_memory_pages();
		if (MACHINE_IS_TIMEX_TS_TC_2068) timex_set_memory_pages();
		if (is_zxuno_chloe_mmu() ) zxuno_set_memory_pages();
		if (MACHINE_IS_TBBLUE) {
			//Sincronizar los 5 bits bajos a registro tbblue

			/*
			(W) 0x69 (105) => DISPLAY CONTROL 1 REGISTER

			Bit	Function
			7	Enable the Layer 2 (alias for Layer 2 Access Port ($123B) bit 1)
			6	Enable ULA shadow (bank 7) display (alias for Memory Paging Control ($7FFD) bit 3)
			5-0	alias for Timex Sinclair Video Mode Control ($xxFF) bits 5:0

			*/
			tbblue_registers[105] &= (128+64);
			tbblue_registers[105] |= (value&63);



		}

	}

}