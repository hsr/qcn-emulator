
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

function rp_init {
	IFACE=$1
	tc qdisc del dev ${IFACE} root;
	tc qdisc add dev ${IFACE} root handle 1: htb default 255;
}

function add_rp {
	if [ -z "$1" ] || [ -z "$2" ] || [ -z "$3" ]; then
		echo "Usage: $0 <IFACE> <IP_SRC> <IP_DST>"
		return;
	fi

	IFACE=$1;
	IP_SRC=$2;
	IP_DST=$3;
	RATE="1000000kbit";
	

	HANDLE=$(gethandle ${IP_SRC} ${IP_DST});

	if [ -z "$(tc qdisc show dev ${IFACE} | grep htb)" ]; then
		echo "Initializing..."
		rp_init ${IFACE};
	fi

	echo "Adding RP Class..."
	tc class add dev ${IFACE} parent root classid 1:${HANDLE} htb \
		rate ${RATE} ceil ${RATE} burst 1500kb cburst 1500kb quantum 3000;

	echo "Adding RP Qdisc..."
	tc qdisc add dev ${IFACE} parent 1:${HANDLE} handle ${HANDLE}: \
		sfq perturb 10;

	echo "Adding RP Filter..."
	tc filter add dev ${IFACE} protocol ip parent 1: prio 1 u32 \
		match ip src ${IP_SRC} match ip dst ${IP_DST} flowid 1:${HANDLE};


}

add_rp $@
