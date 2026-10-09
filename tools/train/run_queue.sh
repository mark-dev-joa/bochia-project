#!/bin/bash
# The GPU takes one training at a time: D-FINE-S after RT-DETR, Faster R-CNN after that.
cd /c/boccia-train
until grep -q "^exit" runs/rtdetr-l-e30.log; do sleep 60; done
echo "queue: rtdetr done $(date +%H:%M)" >> runs/queue.log
cd D-FINE
../.venv/Scripts/python.exe train.py -c configs/dfine/custom/dfine_hgnetv2_s_boccia.yml --use-amp --seed=0 -t ../weights/dfine_s_coco.pth > ../runs/dfine-s.log 2>&1
echo "exit $?" >> ../runs/dfine-s.log
echo "queue: dfine done $(date +%H:%M)" >> ../runs/queue.log
cd ..
./.venv/Scripts/python.exe train_frcnn.py export/20261009-021405-bed880d6 runs/frcnn epochs=12 max_size=1280 batch=4 > runs/frcnn.log 2>&1
echo "exit $?" >> runs/frcnn.log
echo "queue: frcnn done $(date +%H:%M)" >> runs/queue.log
