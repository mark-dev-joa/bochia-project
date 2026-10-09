#!/bin/bash
# Faster R-CNN (train_frcnn.py fixed for Windows' spawned workers), then D-FINE-S again at batch 4 without multi-scale
# (the batch-8 run spilled out of GPU memory). One training at a time on the GPU.
cd /c/boccia-train
echo "queue2: started $(date +%H:%M)" >> runs/queue.log
./.venv/Scripts/python.exe train_frcnn.py export/20261009-021405-bed880d6 runs/frcnn epochs=12 max_size=1280 batch=4 > runs/frcnn.log 2>&1
echo "exit $?" >> runs/frcnn.log
echo "queue2: frcnn done $(date +%H:%M)" >> runs/queue.log
cd D-FINE
../.venv/Scripts/python.exe train.py -c configs/dfine/custom/dfine_hgnetv2_s_boccia_b4.yml --use-amp --seed=0 -t ../weights/dfine_s_coco.pth > ../runs/dfine-s-b4.log 2>&1
echo "exit $?" >> ../runs/dfine-s-b4.log
echo "queue2: dfine done $(date +%H:%M)" >> ../runs/queue.log
