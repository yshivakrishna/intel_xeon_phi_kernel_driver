/*
 * Intel MIC Platform Software Stack (MPSS)
 *
 * Copyright 2010-2012 Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301
 * USA.
 *
 * Disclaimer: The codes contained in these modules may be specific to
 * the Intel Software Development Platform codenamed: Knights Ferry, and
 * the Intel product codenamed: Knights Corner, and are not backward
 * compatible with other Intel products. Additionally, Intel will NOT
 * support the codes or instruction set in future products.
 *
 * Intel offers no warranty of any kind regarding the code. This code is
 * licensed on an "AS IS" basis and Intel is not obligated to provide
 * any support, assistance, installation, training, or other services of
 * any kind. Intel is also not obligated to provide any updates,
 * enhancements or extensions. Intel specifically disclaims any warranty
 * of merchantability, non-infringement, fitness for any particular
 * purpose, and any other warranty.
 *
 * Further, Intel disclaims all liability of any kind, including but not
 * limited to liability for infringement of any proprietary rights,
 * relating to the use of the code, even if Intel is notified of the
 * possibility of such liability. Except as expressly stated in an Intel
 * license agreement provided with this code and agreed upon with Intel,
 * no license, express or implied, by estoppel or otherwise, to any
 * intellectual property rights is granted herein.
 */

#ifndef __MPSSCONFIG_H_
#define __MPSSCONFIG_H_

#define CURRENT_CONFIG_MAJOR 0
#define CURRENT_CONFIG_MINOR 7

#define MPSS_CONFIG_VER(x,y) (((x) << 16) + (y))
#define MPSS_CURRENT_CONFIG_VER ((CURRENT_CONFIG_MAJOR << 16) + CURRENT_CONFIG_MINOR)

#define DEFAULT_CONFIG_DIR "/etc/sysconfig/mic"
#define LOCKFILE_NAME "/var/lock/subsys/mpssd"

#define TRUE	1
#define FALSE	0

#define CONFIG_UNKNOWN		0
#define CONFIG_DISABLED		1
#define CONFIG_ENABLED		2

struct mroot {
	int	type;
#define ROOT_TYPE_UNKNOWN	0
#define ROOT_TYPE_RAMFS		1
#define ROOT_TYPE_STATICRAMFS	2
#define ROOT_TYPE_NFS		3
#define ROOT_TYPE_SPLITNFS	4
#define ROOT_TYPE_INITRD	5
	char	*target;
	char	*nfsusr;
};

struct mnet {
	int	type;
#define NETWORK_TYPE_UNKNOWN	0
#define NETWORK_TYPE_STATPAIR	1
#define NETWORK_TYPE_STATBRIDGE	2
#define NETWORK_TYPE_BRIDGE	3
	char	*hostname;
	char	*bridge;
	char	*gateway;
	char	*micIP;
	char	*hostIP;
	char	*micMac;
	char	*hostMac;
	char	*micMac_dep;
	char	*hostMac_dep;
#define MIC_MAC_SERIAL	((char *)0x1)
#define MIC_MAC_RANDOM	((char *)0x2)
	char	*prefix;
	char	*mtu;
	int	modhosts;
	char	*bridgename_dep;	// Depricated
	char	*subnet_dep;		// Depricated
};

struct source {
	char 	*dir;
	char	*list;
};

struct overdir {
	int		type;
#define OVER_TYPE_SIMPLE	0
#define OVER_TYPE_FILELIST	1
#define OVER_TYPE_FILE		2
	int		state;
	int		level;
	char		*dir;
	char		*target;
	struct overdir	*next;
};

struct mfiles {
	struct source	base;
	struct source	common;
	struct source	mic;
	struct overdir	overlays;
};

struct mservice {
	char *name;
	unsigned int	start;
	unsigned int	stop;
	int		on;
	struct mservice *next;
};

struct mpersist {
	char	*micMac;
	char	*hostMac;
};

struct mboot {
	int	onstart;
	char	*osimage;
	int	verbose;
	char	*extraCmdline;
	char	*console;
	char	*pm;
#ifdef INITRAMFS
	char	*initRamFS;
#endif
};

struct muser {
	char	*method;
	int	low;
	int	high;
};

struct mmisc {
	char	*shutdowntimeout;
	char	*crashdumpDir;
	char	*crashdumplimitgb;
};

struct mconfig {
	int		version;
	struct mmisc	misc;
	struct mboot	boot;
	struct mroot	rootdev;
	struct mfiles	filesrc;
	struct mnet	net;
	struct mservice services;
	struct muser	user;
	struct mpersist	persist;
};

/* Per MIC information used by the MPSSD daemon easily keep here. */
struct mpssdi {
	char		*state;
	scif_epd_t	dep;
	pthread_mutex_t	pth_lock;
	pthread_t	boot_pth;
	pthread_t	download_pth;
	pthread_t	state_pth;
	pthread_t	stop_pth;
	pthread_t	crash_pth;
};

struct mic_info {
	int		id;			// First element - do not move
	char		*name;
	struct mconfig	config;
	struct mpssdi	mpssd;
	struct mic_info *next;
};

extern struct mic_info *mic_list;
extern FILE *logfp;

struct mbridge {
	char		*name;
	unsigned int	type;
#define BRIDGE_TYPE_UNKNOWN	0
#define BRIDGE_TYPE_INT		1
#define BRIDGE_TYPE_EXT		2
#define BRIDGE_TYPE_STATICEXT	3
	char		*ip;
	char		*prefix;
	char 		*mtu;
	struct mbridge	*next;
};

/**
 * mpss_get_miclist - retrieve a linked list of current installed MIC cards
 *
 * mpss_get_miclist() searches the /sys/class/mic directory for installed
 * MIC card entries and returns a linked list of mic_info structures.  Each
 * structure will have an entry for a card with its name and ID Filled out
 * and config structure initialized.

 *\return
 * Upon successful completion mpss_get_miclist returns a pointer to the
 * first element in the chain.  If no MIC cards are found it returns a NULL
 * pointer.
*/
struct mic_info *mpss_get_miclist(void);

/**
 * mpss_parse_config - parse the MPSS config files and fill in config struct
 *     \param mic		Pointer to MIC card info
 *
 * mpss_parse_config() parses the MPSS configuration files for the specified
 * MIC card passed in and fills in it configuration data.

 *\return
 * Upon successful completion mpss_parse_config returns 0.
 *\par Errors:
 *- ENOENT
 * -If the configuration file for the specified mic card could not be found
*/
int mpss_parse_config(struct mic_info *mic, struct mbridge **brlist);

/**
 * mpss_clear_config - clear the MPSS config data structure
 *     \param config		Data to be cleated
 *
 * mpss_clear_config() frees any pointers to data in the structure
 * and sets them to NULL.  Any non pointer data is set to a default value.
*/
void mpss_clear_config(struct mconfig *config);

/**
 * mpss_insert_bridge - add a bridge to the linked list of bridges
 *     \param name		Name of bridge to be added (br0, ...)
 *     \param type		Type of bridge to be added (see struct mbridge)
 *     \param ip		Ip address
 *     \param prefix		Net prefix/netbits to create netmask
 *     \param mtu		MTU of bridge
 *     \param brlist		List to be added to
 *
 * mpss_insert_bridge() adds a bridge to the list of know bridges in
 * "brlist".  It takes all the parameters needed to setup the bridge
 * interface.  If the bridge in empty brlist should point to NULL.

 *\return
 * Upon successful completion mpss_insert_bridge returns 0.
 *\par Errors:
 *- EEXIST
 * - Bridge is already in the list
*/
struct mbridge *mpss_insert_bridge(char *name, int type, char *ip, char *netbits,
				   char *mtu, struct mbridge **brlist);

/**
 * mpss_free_bridgelist - clear the bridge list
 *     \param brlist		Linked list of bridges
 *
 * mpss_free_bridgelist() frees all the malloc memory associated with
 * the bridge list and sets it back to a NULL pointer.
*/
void mpss_free_bridgelist(struct mbridge **brlist);

/**
 * mpss_bridge_byname - find bridge in the list of bridges
 *     \param name		Name of bridge to be found (br0, ...)
 *     \param brlist		Linkes list of bridges
 *
 * mpss_bridge_byname() traverses the list of bridges for the
 * interface <name>.
 *
 *\return
 * Upon successful completion mpss_bridge_byname returns the pointer
 * to its mbridge data structure.  If the name is not found it returns
 * NULL.
*/
struct mbridge *mpss_bridge_byname(char *name, struct mbridge *brlist);

/**
 * mpss_bridge_byip - find bridge in the list of bridges
 *     \param name		Name of bridge to be found (br0, ...)
 *     \param brlist		Linkes list of bridges
 *
 * mpss_bridge_byip() traverses the list of bridges for the IP
 * address <ip>.
 *
 *\return
 * Upon successful completion mpss_bridge_byip returns the pointer
 * to its mbridge data structure.  If the ip is not found it returns
 * NULL.
*/
struct mbridge *mpss_bridge_byip(char *ip, struct mbridge *brlist);

/**
 * mpss_append_config - append a config entry from the confiuration file
 *     \param name		Name of file - ".conf" to remove the line from.
 *     \param single		If false then add qoates to "arg0" argument.
 *     \param descriptor	A string to add above config entry.
 *     \param config		Name of the configuration parameter to add.
 *     \param args		Args 0 - 5 for arguemnts to "config"
 *
 * mpss_append_config() appends configuration lines to the the file
 * /etc/sysconfig/mic/<name>.conf.  It adds a line starting with <config>.  Then
 * it adds the <arg0>.  If single is false then <arg0> will be in quotes.  The
 * arg1 through arg5 are optional and should be set to NULL if not used.
 *
 *\return
 * Upon successful completion mpss_append_config returns 0.
 *\par Errors:
 *- ENOENT
 * -If the configuration file for the specified mic card could not be found
*/
int mpss_append_config(char *name, int single, char *descriptor, char *config, char *arg0,
		       char *arg1, char *arg2, char *arg3, char *arg4, char *arg5);

/**
 * mpss_remove_config - delete a config entry from the confiuration file
 *     \param name		Name of file - ".conf" to remove the line from.
 *     \param line		String of characters to patern match on.
 *     \param len		Max length of data to match on.
 *
 * mpss_remove_config() searches the file /etc/sysconfig/mic/<name>.conf for
 * configuration lines starting with the string <line> for a maximum length
 * defined by <len>.  If it finds one it deletes it from the file.
 *
 *\return
 * Upon successful completion mpss_parse_config returns 0.
 *\par Errors:
 *- ENOENT
 * -If the configuration file for the specified mic card could not be found
*/
int mpss_remove_config(char *name, char *line, int len);

/**
 * mpss_opensysfs - open the sysfs 'entry' under mic card 'name'
 *     \param name		Name of the mic card (i.e. mic0, mic1, ...)
 *     \param entry		Particular sysfs entry under 'name' to open
 *
 * mpss_opensysfs() opens the sysfs entry under /sys/class/mic/<name>/<entry>
 * and returns the file discriptor.
 *
 *\return
 * Upon successful completion an open file descriptor is returned.  If a
 * failure occurs it will return a -1 and errno values will correspond to
 * documentation for open(2).
*/
int mpss_opensysfs(char *name, char *entry);

/**
 * mpss_readsysfs - read the sysfs 'entry' under mic card 'name'
 *     \param name		Name of the mic card (i.e. mic0, mic1, ...)
 *     \param entry		Particular sysfs entry under 'name' to open
 *
 * mpss_readsysfs() reads the output from /sys/class/mic/<name>/<entry>
 * and returns a malloced string including it.  After use is finished the
 * caller should free() this address.
 *
 *\return
 * Upon successful completion an address containg the string of information.
 * If the data could not be read then NULL will be returned.
*/
char *mpss_readsysfs(char *name, char *entry);

/**
 * mpss_setsysfs - open the sysfs 'entry' under mic card 'name'
 *     \param name		Name of the mic card (i.e. mic0, mic1, ...)
 *     \param entry		Particular sysfs entry under 'name' to open
 *     \param value		String to write
 *
 * mpss_setsysfs() opens the sysfs entry under /sys/class/mic/<name>/<entry>
 * and attempts to write the string in 'value' to it.
 *
 *\return
 * Upon successful completion an open file descriptor is returned.  If a
 * failure occurs it will return zero.  If a negative value is returned then
 * the sysfs entry failed to open.  A positive return has the value of errno
 * returned from the write to the entry.
*/
int mpss_setsysfs(char *name, char *entry, char *value);

/**
 * mpss_set_cmdline - create and write the kernel command line
 *     \param mic		mic_info data structre with valid config data
 *
 * mpss_set_cmdline() uses the parsed configuartion data in the mic_info
 * structure passed in to create the kernel command line for the MIC
 * card.  It then writes it to the cards "cmdline" sysfs entry.
 *\return
 * Upon successful completion a zero value is returned.  Non zero value
 * indicates failure.
*/
int mpss_set_cmdline(struct mic_info *mic, struct mbridge *brlist);

#endif /* __MPSSCONFIG_H_ */
