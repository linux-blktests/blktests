#!/bin/sh
# SPDX-License-Identifier: GPL-3.0+
# Copyright (c) 2026 SUSE LLC and contributors
#
# Configure subsystems and ports of a Linux nvmet target through configfs.
# Runs on the target.
#
# Usage: nvmet.sh COMMAND ARGS...
#
#   setup TRTYPE TRADDR TRSVCID TLS NR_PORTS SUBSYSNQN SUBSYS_UUID BLKDEV
#         IMG_SIZE HOSTNQN HOSTKEY CTRLKEY
#	Create a subsystem with namespace 1, allow one host and export the
#	subsystem on NR_PORTS new ports.  Without BLKDEV, the namespace is a
#	loop device on an IMG_SIZE image file, cleanup removes both.
#   cleanup SUBSYSNQN
#	Remove a subsystem, the ports which exported it and no other
#	subsystem, the hosts no other subsystem allows and the loop device
#	and image file setup created.
#   add-ports TRTYPE TRADDR TRSVCID TLS NR_PORTS SUBSYSNQN
#	Export a subsystem on NR_PORTS new ports, print their IDs.
#   add-port TRTYPE TRADDR TRSVCID TLS
#	Create a port which exports no subsystem, print its ID.
#   remove-port PORT
#	Remove a port and the subsystem links and ANA groups on it.
#   list-ports SUBSYSNQN
#	Print the IDs of the ports which export a subsystem.
#   show-port PORT
#	Print the transport type, address and service ID of a port.
#   set-ana PORT GRPID STATE
#	Set the state of an ANA group on a port, create the group if needed.
#   set-host HOSTNQN ATTR VALUE
#	Set a DH-CHAP attribute of a host: dhchap_key, dhchap_ctrl_key,
#	dhchap_hash or dhchap_dhgroup.
#
# A new port gets the lowest free port ID N >= 1 and the service ID
# TRSVCID + N - 1.  TLS is none, not-required or required.

set -e

cfs=/sys/kernel/config/nvmet
state=/var/tmp/nvme-target-control

die() {
	echo "nvmet.sh: $*" >&2
	exit 1
}

# add_port TRTYPE TRADDR TRSVCID TLS: create a port, set its ID in $port
add_port() {
	trtype=$1
	traddr=$2
	trsvcid=$3
	tls=$4

	modprobe nvmet
	modprobe "nvmet-${trtype}"

	port=1
	while [ -e "${cfs}/ports/${port}" ]; do
		port=$((port + 1))
	done
	case "${traddr}" in
	*:*)	adrfam=ipv6 ;;
	*)	adrfam=ipv4 ;;
	esac

	mkdir "${cfs}/ports/${port}"
	echo "${trtype}" > "${cfs}/ports/${port}/addr_trtype"
	echo "${adrfam}" > "${cfs}/ports/${port}/addr_adrfam"
	echo "${traddr}" > "${cfs}/ports/${port}/addr_traddr"
	echo $((trsvcid + port - 1)) > "${cfs}/ports/${port}/addr_trsvcid"
	case "${tls}" in
	none)
		;;
	not-required|required)
		echo tls1.3 > "${cfs}/ports/${port}/addr_tsas"
		if [ "${tls}" = not-required ]; then
			echo "not required" > "${cfs}/ports/${port}/addr_treq"
		fi
		;;
	*)
		die "unknown TLS mode: ${tls}"
		;;
	esac
}

# add_ports TRTYPE TRADDR TRSVCID TLS NR_PORTS SUBSYSNQN
add_ports() {
	[ -d "${cfs}/subsystems/$6" ] || die "no subsystem $6"
	i=0
	while [ "${i}" -lt "$5" ]; do
		add_port "$1" "$2" "$3" "$4"
		# Linking the first subsystem enables the port.
		ln -s "${cfs}/subsystems/$6" "${cfs}/ports/${port}/subsystems/$6"
		echo "${port}"
		i=$((i + 1))
	done
}

remove_port() {
	p="${cfs}/ports/$1"
	[ -d "${p}" ] || die "no port $1"
	for link in "${p}"/subsystems/*; do
		[ -L "${link}" ] && rm "${link}"
	done
	for grp in "${p}"/ana_groups/*; do
		# ANA group 1 belongs to the port.
		[ "${grp##*/}" = 1 ] || rmdir "${grp}"
	done
	rmdir "${p}"
}

setup() {
	trtype=$1
	traddr=$2
	trsvcid=$3
	tls=$4
	nr_ports=$5
	subsysnqn=$6
	subsys_uuid=$7
	blkdev=$8
	img_size=$9
	hostnqn=${10}
	hostkey=${11}
	ctrlkey=${12}

	modprobe nvmet

	if [ -z "${blkdev}" ]; then
		mkdir -p "${state}"
		img="${state}/${subsysnqn}.img"
		rm -f "${img}"
		truncate -s "${img_size}" "${img}"
		blkdev=$(losetup -f --show "${img}")
		echo "${blkdev}" > "${state}/${subsysnqn}.loop"
	fi

	host="${cfs}/hosts/${hostnqn}"
	mkdir -p "${host}"
	if [ -n "${hostkey}" ]; then
		echo "${hostkey}" > "${host}/dhchap_key"
	fi
	if [ -n "${ctrlkey}" ]; then
		echo "${ctrlkey}" > "${host}/dhchap_ctrl_key"
	fi

	subsys="${cfs}/subsystems/${subsysnqn}"
	mkdir "${subsys}"
	echo 0 > "${subsys}/attr_allow_any_host"
	mkdir "${subsys}/namespaces/1"
	echo "${blkdev}" > "${subsys}/namespaces/1/device_path"
	echo "${subsys_uuid}" > "${subsys}/namespaces/1/device_uuid"
	echo 1 > "${subsys}/namespaces/1/enable"
	ln -s "${host}" "${subsys}/allowed_hosts/${hostnqn}"

	add_ports "${trtype}" "${traddr}" "${trsvcid}" "${tls}" \
		"${nr_ports}" "${subsysnqn}" > /dev/null
}

cleanup() {
	subsysnqn=$1
	subsys="${cfs}/subsystems/${subsysnqn}"

	if [ -d "${subsys}" ]; then
		for p in "${cfs}"/ports/*; do
			[ -L "${p}/subsystems/${subsysnqn}" ] || continue
			rm "${p}/subsystems/${subsysnqn}"
			if [ -z "$(ls "${p}/subsystems")" ]; then
				remove_port "${p##*/}"
			fi
		done

		for link in "${subsys}"/allowed_hosts/*; do
			[ -L "${link}" ] || continue
			hostnqn=$(basename "${link}")
			rm "${link}"
			if ! ls "${cfs}"/subsystems/*/allowed_hosts/"${hostnqn}" \
					> /dev/null 2>&1; then
				rmdir "${cfs}/hosts/${hostnqn}"
			fi
		done

		for ns in "${subsys}"/namespaces/*; do
			[ -d "${ns}" ] || continue
			echo 0 > "${ns}/enable"
			rmdir "${ns}"
		done

		rmdir "${subsys}"
	fi

	if [ -f "${state}/${subsysnqn}.loop" ]; then
		losetup -d "$(cat "${state}/${subsysnqn}.loop")"
		rm -f "${state}/${subsysnqn}.loop" "${state}/${subsysnqn}.img"
	fi
}

list_ports() {
	for p in "${cfs}"/ports/*; do
		[ -L "${p}/subsystems/$1" ] && echo "${p##*/}"
	done
	return 0
}

show_port() {
	p="${cfs}/ports/$1"
	[ -d "${p}" ] || die "no port $1"
	echo "$(cat "${p}/addr_trtype") $(cat "${p}/addr_traddr")" \
		"$(cat "${p}/addr_trsvcid")"
}

set_ana() {
	p="${cfs}/ports/$1"
	[ -d "${p}" ] || die "no port $1"
	grp="${p}/ana_groups/$2"
	[ -d "${grp}" ] || mkdir "${grp}"
	echo "$3" > "${grp}/ana_state"
}

set_host() {
	h="${cfs}/hosts/$1"
	[ -d "${h}" ] || die "no host $1"
	case "$2" in
	dhchap_key|dhchap_ctrl_key|dhchap_hash|dhchap_dhgroup)
		echo "$3" > "${h}/$2"
		;;
	*)
		die "unknown host attribute: $2"
		;;
	esac
}

cmd=$1
shift
case "${cmd}" in
setup)		setup "$@" ;;
cleanup)	cleanup "$@" ;;
add-ports)	add_ports "$@" ;;
add-port)	add_port "$@"; echo "${port}" ;;
remove-port)	remove_port "$@" ;;
list-ports)	list_ports "$@" ;;
show-port)	show_port "$@" ;;
set-ana)	set_ana "$@" ;;
set-host)	set_host "$@" ;;
*)		die "unknown command: ${cmd}" ;;
esac
