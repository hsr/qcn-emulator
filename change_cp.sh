#!/bin/bash

function change_cp {
	if [ -z "$1" ]; then
		echo "Usage: $0 <IFACE> <RATE>"
		return;
	fi

	IFACE=$1;
	RATE=$2;
	LIMIT=163840				# Queue size (160KB)
	
	if [ -z "$(tc qdisc show dev ${IFACE} | grep tbf)" ]; then
		echo "No cp at dev ${IFACE}!"
		return
	fi
	
	tc qdisc change dev ${IFACE} root tbf rate ${RATE} burst 1500kb limit ${LIMIT}
}

change_cp $@