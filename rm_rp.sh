#!/bin/bash

function substrr {
    STR=$1;
	IDX=$[$2+1];
	echo ${STR:$[${#STR[@]}-${IDX}]};
}

function gethostpart {
	IP=$1;
	IDX=1;
	while [ "$(substrr ${IP} ${IDX} | awk '{print index($0, ".")}')" == "0" ]; do
		IDX=$[${IDX}+1];
	done;	
	HOSTP=$(echo $(substrr ${IP} $[${IDX}-1]) | xargs printf "%0.2x")
	echo ${HOSTP}
}

function gethandle {
	IP_SRC=$1;
	IP_DST=$2;
	
	SRC=$(gethostpart "${IP_SRC}")
	DST=$(gethostpart "${IP_DST}")
	echo ${SRC}${DST}

}

function rm_rp {
	if [ -z "$1" ] || [ -z "$2" ] || [ -z "$3" ]; then
		echo "Usage: $0 <IFACE> <IP_SRC> <IP_DST>"
		return;
	fi

	IFACE=$1;
	IP_SRC=$2;
	IP_DST=$3;
	RATE="1000000kbit";

	HANDLE=$(gethandle ${IP_SRC} ${IP_DST});

	FILTERID=$(tc filter show dev ${IFACE} | grep ${HANDLE} | cut -d ' ' -f 10)
	if [ ! -z "${FILTERID}" ]; then
		tc filter del dev ${IFACE} protocol ip parent 1: prio 1 \
			handle ${FILTERID} u32;
	fi

	if [ ! -z "$(tc qdisc show dev ${IFACE} | grep ${HANDLE})" ]; then	
		tc qdisc del dev ${IFACE} parent 1:${HANDLE} handle ${HANDLE}: sfq
	fi

	if [ ! -z "$(tc class show dev ${IFACE} | grep ${HANDLE})" ]; then	
		tc class del dev ${IFACE} parent root classid 1:${HANDLE} htb \
			rate ${RATE} ceil ${RATE} burst 1500kb cburst 1500kb;
	fi

}

rm_rp $@