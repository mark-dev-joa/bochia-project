@echo off
cd /d "%~dp0"
python ball_labeler.py --auto %*
if errorlevel 1 pause
