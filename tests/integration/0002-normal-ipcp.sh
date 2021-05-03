#!/bin/bash -e

# Create a normal IPCP
rlite-ctl ipcp-create x normal xx
# Check that kernel knows it's there
rlite-ctl | grep dif_name | grep "\<x\>" | grep "\<xx\>"
# Check that it responds to RIB queries
rlite-ctl dif-rib-show xx
rlite-ctl dif-routing-show xx
rlite-ctl dif-rib-paths-show xx
