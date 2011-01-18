#!/bin/bash

function add_cp {
	if [ -z "$1" ]; then
		echo "Usage: $0 <IFACE> <RATE>"
		return;
	fi

	IFACE=$1;
	RATE=$2;
	LIMIT=163840				# Queue size (160KB)
	
	if [ ! -z "$(tc qdisc show dev ${IFACE} | grep tbf)" ]; then
		echo "Initializing..."
		tc qdisc del dev ${IFACE} root
	fi
	
	tc qdisc add dev ${IFACE} root tbf rate ${RATE} burst 1500kb limit ${LIMIT}

}

add_cp $@
