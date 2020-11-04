/*
 * Copyright (C) 2008 Michael Brown <mbrown@fensystems.co.uk>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 * You can also choose to distribute this program under the terms of
 * the Unmodified Binary Distribution Licence (as given in the file
 * COPYING.UBDL), provided that you have satisfied its requirements.
 */

FILE_LICENCE ( GPL2_OR_LATER_OR_UBDL );

#include <stdlib.h>
#include <errno.h>
#include <ipxe/pci.h>
#include <ipxe/acpi.h>
#include <ipxe/efi/efi.h>
#include <ipxe/efi/efi_pci.h>
#include <ipxe/efi/efi_driver.h>
#include <ipxe/efi/Protocol/PciIo.h>
#include <ipxe/efi/Protocol/PciRootBridgeIo.h>

/** @file
 *
 * iPXE PCI I/O API for EFI
 *
 */

/* Disambiguate the various error causes */
#define EINFO_EEFI_PCI							\
	__einfo_uniqify ( EINFO_EPLATFORM, 0x01,			\
			  "Could not open PCI I/O protocol" )
#define EINFO_EEFI_PCI_NOT_PCI						\
	__einfo_platformify ( EINFO_EEFI_PCI, EFI_UNSUPPORTED,		\
			      "Not a PCI device" )
#define EEFI_PCI_NOT_PCI __einfo_error ( EINFO_EEFI_PCI_NOT_PCI )
#define EINFO_EEFI_PCI_IN_USE						\
	__einfo_platformify ( EINFO_EEFI_PCI, EFI_ACCESS_DENIED,	\
			      "PCI device already has a driver" )
#define EEFI_PCI_IN_USE __einfo_error ( EINFO_EEFI_PCI_IN_USE )
#define EEFI_PCI( efirc )						\
	EPLATFORM ( EINFO_EEFI_PCI, efirc,				\
		    EEFI_PCI_NOT_PCI, EEFI_PCI_IN_USE )

/******************************************************************************
 *
 * iPXE PCI API
 *
 ******************************************************************************
 */

/**
 * Open EFI PCI root bridge I/O protocol
 *
 * @v pci		PCI device
 * @ret handle		EFI PCI root bridge handle
 * @ret root		EFI PCI root bridge I/O protocol, or NULL if not found
 * @ret rc		Return status code
 */
static int efipci_root_open ( struct pci_device *pci, EFI_HANDLE *handle,
			      EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL **root ) {
	EFI_BOOT_SERVICES *bs = efi_systab->BootServices;
	EFI_HANDLE *handles;
	UINTN num_handles;
	union {
		void *interface;
		EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *root;
	} u;
	EFI_STATUS efirc;
	UINTN i;
	int rc;

	/* Enumerate all handles */
	if ( ( efirc = bs->LocateHandleBuffer ( ByProtocol,
			&efi_pci_root_bridge_io_protocol_guid,
			NULL, &num_handles, &handles ) ) != 0 ) {
		rc = -EEFI ( efirc );
		DBGC ( pci, "EFIPCI " PCI_FMT " cannot locate root bridges: "
		       "%s\n", PCI_ARGS ( pci ), strerror ( rc ) );
		goto err_locate;
	}

	/* Look for matching root bridge I/O protocol */
	for ( i = 0 ; i < num_handles ; i++ ) {
		*handle = handles[i];
		if ( ( efirc = bs->OpenProtocol ( *handle,
				&efi_pci_root_bridge_io_protocol_guid,
				&u.interface, efi_image_handle, *handle,
				EFI_OPEN_PROTOCOL_GET_PROTOCOL ) ) != 0 ) {
			rc = -EEFI ( efirc );
			DBGC ( pci, "EFIPCI " PCI_FMT " cannot open %s: %s\n",
			       PCI_ARGS ( pci ), efi_handle_name ( *handle ),
			       strerror ( rc ) );
			continue;
		}
		if ( u.root->SegmentNumber == PCI_SEG ( pci->busdevfn ) ) {
			*root = u.root;
			bs->FreePool ( handles );
			return 0;
		}
		bs->CloseProtocol ( *handle,
				    &efi_pci_root_bridge_io_protocol_guid,
				    efi_image_handle, *handle );
	}
	DBGC ( pci, "EFIPCI " PCI_FMT " found no root bridge\n",
	       PCI_ARGS ( pci ) );
	rc = -ENOENT;

	bs->FreePool ( handles );
 err_locate:
	return rc;
}

/**
 * Close EFI PCI root bridge I/O protocol
 *
 * @v handle		EFI PCI root bridge handle
 */
static void efipci_root_close ( EFI_HANDLE handle ) {
	EFI_BOOT_SERVICES *bs = efi_systab->BootServices;

	/* Close protocol */
	bs->CloseProtocol ( handle, &efi_pci_root_bridge_io_protocol_guid,
			    efi_image_handle, handle );
}

/**
 * Calculate EFI PCI configuration space address
 *
 * @v pci		PCI device
 * @v location		Encoded offset and width
 * @ret address		EFI PCI address
 */
static unsigned long efipci_address ( struct pci_device *pci,
				      unsigned long location ) {

	return EFI_PCI_ADDRESS ( PCI_BUS ( pci->busdevfn ),
				 PCI_SLOT ( pci->busdevfn ),
				 PCI_FUNC ( pci->busdevfn ),
				 EFIPCI_OFFSET ( location ) );
}

/**
 * Read from PCI configuration space
 *
 * @v pci		PCI device
 * @v location		Encoded offset and width
 * @ret value		Value
 * @ret rc		Return status code
 */
int efipci_read ( struct pci_device *pci, unsigned long location,
		  void *value ) {
	EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *root;
	EFI_HANDLE handle;
	EFI_STATUS efirc;
	int rc;

	/* Open root bridge */
	if ( ( rc = efipci_root_open ( pci, &handle, &root ) ) != 0 )
		goto err_root;

	/* Read from configuration space */
	if ( ( efirc = root->Pci.Read ( root, EFIPCI_WIDTH ( location ),
					efipci_address ( pci, location ), 1,
					value ) ) != 0 ) {
		rc = -EEFI ( efirc );
		DBGC ( pci, "EFIPCI " PCI_FMT " config read from offset %02lx "
		       "failed: %s\n", PCI_ARGS ( pci ),
		       EFIPCI_OFFSET ( location ), strerror ( rc ) );
		goto err_read;
	}

 err_read:
	efipci_root_close ( handle );
 err_root:
	return rc;
}

/**
 * Write to PCI configuration space
 *
 * @v pci		PCI device
 * @v location		Encoded offset and width
 * @v value		Value
 * @ret rc		Return status code
 */
int efipci_write ( struct pci_device *pci, unsigned long location,
		   unsigned long value ) {
	EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *root;
	EFI_HANDLE handle;
	EFI_STATUS efirc;
	int rc;

	/* Open root bridge */
	if ( ( rc = efipci_root_open ( pci, &handle, &root ) ) != 0 )
		goto err_root;

	/* Read from configuration space */
	if ( ( efirc = root->Pci.Write ( root, EFIPCI_WIDTH ( location ),
					 efipci_address ( pci, location ), 1,
					 &value ) ) != 0 ) {
		rc = -EEFI ( efirc );
		DBGC ( pci, "EFIPCI " PCI_FMT " config write to offset %02lx "
		       "failed: %s\n", PCI_ARGS ( pci ),
		       EFIPCI_OFFSET ( location ), strerror ( rc ) );
		goto err_write;
	}

 err_write:
	efipci_root_close ( handle );
 err_root:
	return rc;
}

/**
 * Map PCI bus address as an I/O address
 *
 * @v bus_addr		PCI bus address
 * @v len		Length of region
 * @ret io_addr		I/O address, or NULL on error
 */
void * efipci_ioremap ( struct pci_device *pci, unsigned long bus_addr,
			size_t len ) {
	union {
		union acpi_resource *res;
		void *raw;
	} u;
	EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *root;
	EFI_HANDLE handle;
	unsigned int tag;
	uint64_t offset;
	uint64_t start;
	uint64_t end;
	EFI_STATUS efirc;
	int rc;

	/* Open root bridge */
	if ( ( rc = efipci_root_open ( pci, &handle, &root ) ) != 0 )
		goto err_root;

	/* Get ACPI resource descriptors */
	if ( ( efirc = root->Configuration ( root, &u.raw ) ) != 0 ) {
		rc = -EEFI ( efirc );
		DBGC ( pci, "EFIPCI " PCI_FMT " cannot get configuration: "
		       "%s\n", PCI_ARGS ( pci ), strerror ( rc ) );
		goto err_config;
	}

	/* Parse resource descriptors */
	for ( ; ( ( tag = acpi_resource_tag ( u.res ) ) != ACPI_END_RESOURCE ) ;
	      u.res = acpi_resource_next ( u.res ) ) {

		/* Ignore anything other than an address space descriptor */
		if ( tag != ACPI_QWORD_ADDRESS_SPACE_RESOURCE )
			continue;

		/* Ignore descriptors that do not cover this memory range */
		if ( u.res->qword.type != ACPI_ADDRESS_TYPE_MEM )
			continue;
		offset = le64_to_cpu ( u.res->qword.offset );
		start = ( offset + le64_to_cpu ( u.res->qword.min ) );
		end = ( start + le64_to_cpu ( u.res->qword.len ) );
		if ( ( bus_addr < start ) || ( ( bus_addr + len ) > end ) )
			continue;

		/* Use this address space descriptor */
		DBGC2 ( pci, "EFIPCI " PCI_FMT " %08lx+%zx -> ",
			PCI_ARGS ( pci ), bus_addr, len );
		bus_addr -= offset;
		DBGC2 ( pci, "%08lx\n", bus_addr );
		break;
	}
	if ( tag == ACPI_END_RESOURCE ) {
		DBGC ( pci, "EFIPCI " PCI_FMT " %08lx+%zx is not within "
		       "root bridge address space\n",
		       PCI_ARGS ( pci ), bus_addr, len );
	}

 err_config:
	efipci_root_close ( handle );
 err_root:
	return ioremap ( bus_addr, len );
}

PROVIDE_PCIAPI_INLINE ( efi, pci_num_bus );
PROVIDE_PCIAPI_INLINE ( efi, pci_read_config_byte );
PROVIDE_PCIAPI_INLINE ( efi, pci_read_config_word );
PROVIDE_PCIAPI_INLINE ( efi, pci_read_config_dword );
PROVIDE_PCIAPI_INLINE ( efi, pci_write_config_byte );
PROVIDE_PCIAPI_INLINE ( efi, pci_write_config_word );
PROVIDE_PCIAPI_INLINE ( efi, pci_write_config_dword );
PROVIDE_PCIAPI ( efi, pci_ioremap, efipci_ioremap );

/******************************************************************************
 *
 * EFI PCI device instantiation
 *
 ******************************************************************************
 */

/**
 * Open EFI PCI device
 *
 * @v device		EFI device handle
 * @v attributes	Protocol opening attributes
 * @v efipci		EFI PCI device to fill in
 * @ret rc		Return status code
 */
int efipci_open ( EFI_HANDLE device, UINT32 attributes,
		  struct efi_pci_device *efipci ) {
	EFI_BOOT_SERVICES *bs = efi_systab->BootServices;
	union {
		EFI_PCI_IO_PROTOCOL *pci_io;
		void *interface;
	} pci_io;
	UINTN pci_segment, pci_bus, pci_dev, pci_fn;
	unsigned int busdevfn;
	EFI_STATUS efirc;
	int rc;

	/* See if device is a PCI device */
	if ( ( efirc = bs->OpenProtocol ( device, &efi_pci_io_protocol_guid,
					  &pci_io.interface, efi_image_handle,
					  device, attributes ) ) != 0 ) {
		rc = -EEFI_PCI ( efirc );
		DBGCP ( device, "EFIPCI %s cannot open PCI protocols: %s\n",
			efi_handle_name ( device ), strerror ( rc ) );
		goto err_open_protocol;
	}
	efipci->io = pci_io.pci_io;

	/* Get PCI bus:dev.fn address */
	if ( ( efirc = pci_io.pci_io->GetLocation ( pci_io.pci_io, &pci_segment,
						    &pci_bus, &pci_dev,
						    &pci_fn ) ) != 0 ) {
		rc = -EEFI ( efirc );
		DBGC ( device, "EFIPCI %s could not get PCI location: %s\n",
		       efi_handle_name ( device ), strerror ( rc ) );
		goto err_get_location;
	}
	busdevfn = PCI_BUSDEVFN ( pci_segment, pci_bus, pci_dev, pci_fn );
	pci_init ( &efipci->pci, busdevfn );
	DBGCP ( device, "EFIPCI " PCI_FMT " is %s\n",
		PCI_ARGS ( &efipci->pci ), efi_handle_name ( device ) );

	/* Try to enable I/O cycles, memory cycles, and bus mastering.
	 * Some platforms will 'helpfully' report errors if these bits
	 * can't be enabled (for example, if the card doesn't actually
	 * support I/O cycles).  Work around any such platforms by
	 * enabling bits individually and simply ignoring any errors.
	 */
	pci_io.pci_io->Attributes ( pci_io.pci_io,
				    EfiPciIoAttributeOperationEnable,
				    EFI_PCI_IO_ATTRIBUTE_IO, NULL );
	pci_io.pci_io->Attributes ( pci_io.pci_io,
				    EfiPciIoAttributeOperationEnable,
				    EFI_PCI_IO_ATTRIBUTE_MEMORY, NULL );
	pci_io.pci_io->Attributes ( pci_io.pci_io,
				    EfiPciIoAttributeOperationEnable,
				    EFI_PCI_IO_ATTRIBUTE_BUS_MASTER, NULL );

	/* Populate PCI device */
	if ( ( rc = pci_read_config ( &efipci->pci ) ) != 0 ) {
		DBGC ( device, "EFIPCI " PCI_FMT " cannot read PCI "
		       "configuration: %s\n",
		       PCI_ARGS ( &efipci->pci ), strerror ( rc ) );
		goto err_pci_read_config;
	}

	return 0;

 err_pci_read_config:
 err_get_location:
	bs->CloseProtocol ( device, &efi_pci_io_protocol_guid,
			    efi_image_handle, device );
 err_open_protocol:
	return rc;
}

/**
 * Close EFI PCI device
 *
 * @v device		EFI device handle
 */
void efipci_close ( EFI_HANDLE device ) {
	EFI_BOOT_SERVICES *bs = efi_systab->BootServices;

	bs->CloseProtocol ( device, &efi_pci_io_protocol_guid,
			    efi_image_handle, device );
}

/**
 * Get EFI PCI device information
 *
 * @v device		EFI device handle
 * @v efipci		EFI PCI device to fill in
 * @ret rc		Return status code
 */
int efipci_info ( EFI_HANDLE device, struct efi_pci_device *efipci ) {
	int rc;

	/* Open PCI device, if possible */
	if ( ( rc = efipci_open ( device, EFI_OPEN_PROTOCOL_GET_PROTOCOL,
				  efipci ) ) != 0 )
		return rc;

	/* Close PCI device */
	efipci_close ( device );

	return 0;
}

/******************************************************************************
 *
 * EFI PCI driver
 *
 ******************************************************************************
 */

/**
 * Check to see if driver supports a device
 *
 * @v device		EFI device handle
 * @ret rc		Return status code
 */
static int efipci_supported ( EFI_HANDLE device ) {
	struct efi_pci_device efipci;
	int rc;

	/* Get PCI device information */
	if ( ( rc = efipci_info ( device, &efipci ) ) != 0 )
		return rc;

	/* Look for a driver */
	if ( ( rc = pci_find_driver ( &efipci.pci ) ) != 0 ) {
		DBGC ( device, "EFIPCI " PCI_FMT " (%04x:%04x class %06x) "
		       "has no driver\n", PCI_ARGS ( &efipci.pci ),
		       efipci.pci.vendor, efipci.pci.device,
		       efipci.pci.class );
		return rc;
	}
	DBGC ( device, "EFIPCI " PCI_FMT " (%04x:%04x class %06x) has driver "
	       "\"%s\"\n", PCI_ARGS ( &efipci.pci ), efipci.pci.vendor,
	       efipci.pci.device, efipci.pci.class, efipci.pci.id->name );

	return 0;
}

/**
 * Attach driver to device
 *
 * @v efidev		EFI device
 * @ret rc		Return status code
 */
static int efipci_start ( struct efi_device *efidev ) {
	EFI_HANDLE device = efidev->device;
	struct efi_pci_device *efipci;
	int rc;

	/* Allocate PCI device */
	efipci = zalloc ( sizeof ( *efipci ) );
	if ( ! efipci ) {
		rc = -ENOMEM;
		goto err_alloc;
	}

	/* Open PCI device */
	if ( ( rc = efipci_open ( device, ( EFI_OPEN_PROTOCOL_BY_DRIVER |
					    EFI_OPEN_PROTOCOL_EXCLUSIVE ),
				  efipci ) ) != 0 ) {
		DBGC ( device, "EFIPCI %s could not open PCI device: %s\n",
		       efi_handle_name ( device ), strerror ( rc ) );
		DBGC_EFI_OPENERS ( device, device, &efi_pci_io_protocol_guid );
		goto err_open;
	}

	/* Find driver */
	if ( ( rc = pci_find_driver ( &efipci->pci ) ) != 0 ) {
		DBGC ( device, "EFIPCI " PCI_FMT " has no driver\n",
		       PCI_ARGS ( &efipci->pci ) );
		goto err_find_driver;
	}

	/* Mark PCI device as a child of the EFI device */
	efipci->pci.dev.parent = &efidev->dev;
	list_add ( &efipci->pci.dev.siblings, &efidev->dev.children );

	/* Probe driver */
	if ( ( rc = pci_probe ( &efipci->pci ) ) != 0 ) {
		DBGC ( device, "EFIPCI " PCI_FMT " could not probe driver "
		       "\"%s\": %s\n", PCI_ARGS ( &efipci->pci ),
		       efipci->pci.id->name, strerror ( rc ) );
		goto err_probe;
	}
	DBGC ( device, "EFIPCI " PCI_FMT " using driver \"%s\"\n",
	       PCI_ARGS ( &efipci->pci ), efipci->pci.id->name );

	efidev_set_drvdata ( efidev, efipci );
	return 0;

	pci_remove ( &efipci->pci );
 err_probe:
	list_del ( &efipci->pci.dev.siblings );
 err_find_driver:
	efipci_close ( device );
 err_open:
	free ( efipci );
 err_alloc:
	return rc;
}

/**
 * Detach driver from device
 *
 * @v efidev		EFI device
  */
static void efipci_stop ( struct efi_device *efidev ) {
	struct efi_pci_device *efipci = efidev_get_drvdata ( efidev );
	EFI_HANDLE device = efidev->device;

	pci_remove ( &efipci->pci );
	list_del ( &efipci->pci.dev.siblings );
	efipci_close ( device );
	free ( efipci );
}

/** EFI PCI driver */
struct efi_driver efipci_driver __efi_driver ( EFI_DRIVER_NORMAL ) = {
	.name = "PCI",
	.supported = efipci_supported,
	.start = efipci_start,
	.stop = efipci_stop,
};
