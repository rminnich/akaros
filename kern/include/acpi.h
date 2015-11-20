/* 
 * This file is part of the UCB release of Plan 9. It is subject to the license
 * terms in the LICENSE file found in the top-level directory of this
 * distribution and at http://akaros.cs.berkeley.edu/files/Plan9License. No
 * part of the UCB release of Plan 9, including this file, may be copied,
 * modified, propagated, or distributed except according to the terms contained
 * in the LICENSE file.
 */

#ifndef ROS_KERN_ACPI_H
#define ROS_KERN_ACPI_H

enum {

	Sdthdrsz = 36,				/* size of SDT header */

	/* ACPI regions. Gas ids */
	Rsysmem = 0,
	Rsysio,
	Rpcicfg,
	Rembed,
	Rsmbus,
	Rcmos,
	Rpcibar,
	Ripmi,
	Rfixedhw = 0x7f,

	/* ACPI PM1 control */
	Pm1SciEn = 0x1,	/* Generate SCI and not SMI */

	/* ACPI tbdf as encoded in acpi region base addresses */
	Rpciregshift = 0,
	Rpciregmask = 0xFFFF,
	Rpcifunshift = 16,
	Rpcifunmask = 0xFFFF,
	Rpcidevshift = 32,
	Rpcidevmask = 0xFFFF,
	Rpcibusshift = 48,
	Rpcibusmask = 0xFFFF,

	/* Apic structure types */
	ASlapic = 0,	/* processor local apic */
	ASioapic,	/* I/O apic */
	ASintovr,	/* Interrupt source override */
	ASnmi,	/* NMI source */
	ASlnmi,	/* local apic nmi */
	ASladdr,	/* local apic address override */
	ASiosapic,	/* I/O sapic */
	ASlsapic,	/* local sapic */
	ASintsrc,	/* platform interrupt sources */
	ASlx2apic,	/* local x2 apic */
	ASlx2nmi,	/* local x2 apic NMI */

	/* Apic flags */
	AFbus = 0,	/* polarity/trigger like in ISA */
	AFhigh = 1,	/* active high */
	AFlow = 3,	/* active low */
	AFpmask = 3,	/* polarity bits */
	AFedge = 1 << 2,	/* edge triggered */
	AFlevel = 3 << 2,	/* level triggered */
	AFtmask = 3 << 2,	/* trigger bits */

	/* SRAT types */
	SRlapic = 0,	/* Local apic/sapic affinity */
	SRmem,	/* Memory affinity */
	SRlx2apic,	/* x2 apic affinity */

	/* DMAR types */
	DRHD = 0,
	RMRR,
	ATSR,
	RHSA,
	ANDD,

	/* Arg for _PIC */
	Ppic = 0,	/* PIC interrupt model */
	Papic,	/* APIC interrupt model */
	Psapic,	/* SAPIC interrupt model */

	CMregion = 0,	/* regio name spc base len accsz */
	CMgpe,	/* gpe name id */
};

/*
 * ACPI table (sw)
 * includes pointers to functions used to parse and dump them. 
 */
struct Atable {
	struct Atable *read(uint8_t * p, int len);
	char *pretty(char *start, char *end, struct Dmar *dt);
	char *raw(char *start, char *end, struct Dmar *dt);
	struct Atable *next;		/* next table in list */
	int is64;					/* uses 64bits */
	char sig[5];				/* signature */
	char oemid[7];				/* oem id str. */
	char oemtblid[9];			/* oem tbl. id str. */
	uint8_t *tbl;				/* pointer to table in memory */
	long dlen;					/* size of data in table, after Stdhdr */
};

struct Gpe {
	uintptr_t stsio;			/* port used for status */
	int stsbit;					/* bit number */
	uintptr_t enio;				/* port used for enable */
	int enbit;					/* bit number */
	int nb;						/* event number */
	char *obj;					/* handler object  */
	int id;						/* id as supplied by user */
};

struct Parse {
	char *sig;
	struct Atable *(*f) (uint8_t * unused_uint8_p_t, int);	/* return NULL to keep vmap */
};

struct Regio {
	void *arg;
	 uint8_t(*get8) (uintptr_t, void *);
	void (*set8) (uintptr_t, uint8_t unused_int, void *);
	 uint16_t(*get16) (uintptr_t, void *);
	void (*set16) (uintptr_t, uint16_t unused_int, void *);
	 uint32_t(*get32) (uintptr_t, void *);
	void (*set32) (uintptr_t, uint32_t, void *);
	 uint64_t(*get64) (uintptr_t, void *);
	void (*set64) (uintptr_t, uint64_t unused_int, void *);
};

struct Reg {
	char *name;
	int spc;					/* io space */
	uint64_t base;				/* address, physical */
	uint8_t *p;					/* address, kmapped */
	uint64_t len;
	int tbdf;
	int accsz;					/* access size */
};

/* Generic address structure. 
 */
struct Gas {
	uint8_t spc;				/* address space id */
	uint8_t len;				/* register size in bits */
	uint8_t off;				/* bit offset */
	uint8_t accsz;				/* 1: byte; 2: word; 3: dword; 4: qword */
	uint64_t addr;				/* address (or acpi encoded tbdf + reg) */
};

/* Root system description table pointer.
 * Used to locate the root system description table RSDT
 * (or the extended system description table from version 2) XSDT.
 * The XDST contains (after the DST header) a list of pointers to tables:
 *	- FADT	fixed acpi description table.
 *		It points to the DSDT, AML code making the acpi namespace.
 *	- SSDTs	tables with AML code to add to the acpi namespace.
 *	- pointers to other tables for apics, etc.
 */

struct Rsdp {
	uint8_t signature[8];		/* "RSD PTR " */
	uint8_t rchecksum;
	uint8_t oemid[6];
	uint8_t revision;
	uint8_t raddr[4];			/* RSDT */
	uint8_t length[4];
	uint8_t xaddr[8];			/* XSDT */
	uint8_t xchecksum;			/* XSDT */
	uint8_t _33_[3];			/* reserved */
};

/* Header for ACPI description tables
 */
struct Sdthdr {
	uint8_t sig[4];				/* "FACP" or whatever */
	uint8_t length[4];
	uint8_t rev;
	uint8_t csum;
	uint8_t oemid[6];
	uint8_t oemtblid[8];
	uint8_t oemrev[4];
	uint8_t creatorid[4];
	uint8_t creatorrev[4];
};

/* Firmware control structure
 */
struct Facs {
	uint32_t hwsig;
	uint32_t wakingv;
	uint32_t glock;
	uint32_t flags;
	uint64_t xwakingv;
	uint8_t vers;
	uint32_t ospmflags;
};

/* Maximum System Characteristics table
 */
struct Msct {
	int ndoms;					/* number of domains */
	int nclkdoms;				/* number of clock domains */
	uint64_t maxpa;				/* max physical address */

	struct Mdom *dom;			/* domain information list */
};

struct Mdom {
	struct Mdom *next;
	int start;					/* start dom id */
	int end;					/* end dom id */
	int maxproc;				/* max processor capacity */
	uint64_t maxmem;			/* max memory capacity */
};

/* Multiple APIC description table
 * Interrupts are virtualized by ACPI and each APIC has
 * a `virtual interrupt base' where its interrupts start.
 * Addresses are processor-relative physical addresses.
 * Only enabled devices are linked, others are filtered out.
 */
struct Madt {
	uint64_t lapicpa;			/* local APIC addr */
	int pcat;					/* the machine has PC/AT 8259s */
	struct Apicst *st;			/* list of Apic related structures */
};

struct Apicst {
	int type;
	struct Apicst *next;
	union {
		struct {
			int pid;			/* processor id */
			int id;				/* apic no */
		} lapic;
		struct {
			int id;				/* io apic id */
			uint32_t ibase;		/* interrupt base addr. */
			uint64_t addr;		/* base address */
		} ioapic, iosapic;
		struct {
			int irq;			/* bus intr. source (ISA only) */
			int intr;			/* system interrupt */
			int flags;			/* apic flags */
		} intovr;
		struct {
			int intr;			/* system interrupt */
			int flags;			/* apic flags */
		} nmi;
		struct {
			int pid;			/* processor id */
			int flags;			/* lapic flags */
			int lint;			/* lapic LINTn for nmi */
		} lnmi;
		struct {
			int pid;			/* processor id */
			int id;				/* apic id */
			int eid;			/* apic eid */
			int puid;			/* processor uid */
			char *puids;		/* same thing */
		} lsapic;
		struct {
			int pid;			/* processor id */
			int peid;			/* processor eid */
			int iosv;			/* io sapic vector */
			int intr;			/* global sys intr. */
			int type;			/* intr type */
			int flags;			/* apic flags */
			int any;			/* err sts at any proc */
		} intsrc;
		struct {
			int id;				/* x2 apic id */
			int puid;			/* processor uid */
		} lx2apic;
		struct {
			int puid;
			int flags;
			int intr;
		} lx2nmi;
	};
};

/* System resource affinity table
 */
struct Srat {
	int type;
	struct Srat *next;
	union {
		struct {
			int dom;			/* proximity domain */
			int apic;			/* apic id */
			int sapic;			/* sapic id */
			int clkdom;			/* clock domain */
		} lapic;
		struct {
			int dom;			/* proximity domain */
			uint64_t addr;		/* base address */
			uint64_t len;
			int hplug;			/* hot pluggable */
			int nvram;			/* non volatile */
		} mem;
		struct {
			int dom;			/* proximity domain */
			int apic;			/* x2 apic id */
			int clkdom;			/* clock domain */
		} lx2apic;
	};
};

/* System locality information table
 */
struct Slit {
	uint64_t rowlen;
	struct SlEntry **e;
};

struct SlEntry {
	int dom;					/* proximity domain */
	unsigned int dist;			/* distance to proximity domain */
};

/* Fixed ACPI description table.
 * Describes implementation and hardware registers.
 * PM* blocks are low level functions.
 * GPE* blocks refer to general purpose events.
 * P_* blocks are for processor features.
 * Has address for the DSDT.
 */
struct Fadt {
	uint32_t facs;
	uint32_t dsdt;
	/* 1 reserved */
	uint8_t pmprofile;
	uint16_t sciint;
	uint32_t smicmd;
	uint8_t acpienable;
	uint8_t acpidisable;
	uint8_t s4biosreq;
	uint8_t pstatecnt;
	uint32_t pm1aevtblk;
	uint32_t pm1bevtblk;
	uint32_t pm1acntblk;
	uint32_t pm1bcntblk;
	uint32_t pm2cntblk;
	uint32_t pmtmrblk;
	uint32_t gpe0blk;
	uint32_t gpe1blk;
	uint8_t pm1evtlen;
	uint8_t pm1cntlen;
	uint8_t pm2cntlen;
	uint8_t pmtmrlen;
	uint8_t gpe0blklen;
	uint8_t gpe1blklen;
	uint8_t gp1base;
	uint8_t cstcnt;
	uint16_t plvl2lat;
	uint16_t plvl3lat;
	uint16_t flushsz;
	uint16_t flushstride;
	uint8_t dutyoff;
	uint8_t dutywidth;
	uint8_t dayalrm;
	uint8_t monalrm;
	uint8_t century;
	uint16_t iapcbootarch;
	/* 1 reserved */
	uint32_t flags;
	struct Gas resetreg;
	uint8_t resetval;
	/* 3 reserved */
	uint64_t xfacs;
	uint64_t xdsdt;
	struct Gas xpm1aevtblk;
	struct Gas xpm1bevtblk;
	struct Gas xpm1acntblk;
	struct Gas xpm1bcntblk;
	struct Gas xpm2cntblk;
	struct Gas xpmtmrblk;
	struct Gas xgpe0blk;
	struct Gas xgpe1blk;
};

/* XSDT/RSDT. 4/8 byte addresses starting at p.
 */
struct Xsdt {
	int len;
	int asize;
	uint8_t *p;
};

/* DMAR. 
 */
/* The device scope is basic tbdf as uint32_t. There is a special value that means
 * "everything" and if we see that we set "all" in the Drhd. 
 */
struct DeviceScope {
	uint32_t tbdf;
};

/* this is just hateful, and maybe there's a better way.
 * I can't think of anything that's not a total ugly clusterfuck.
 */
struct Dtab {
	int type;
	union {
		struct Drhd {
			int nscope;
			int segment;
			uintptr_t base;
			uintptr_t all; // this drhd scope is for everything.
			struct DeviceScope scopes[];
		}drhd;
	};
};

struct Dmar {
	int haw;
	/* no, sorry, if your stupid firmware disables x2apic
	 * mode, you should not be here. We ignore that bit.
	 */
	int intr_remap;
	int numentry;
	struct Dtab dtab[];
};

extern uintptr_t acpimblocksize(uintptr_t, int *);

int acpiinit(void);
struct Atable *new_acpi_table(uint8_t *p);
extern struct Madt *apics;
extern struct Srat *srat;

#endif /* ROS_KERN_ACPI_H */
