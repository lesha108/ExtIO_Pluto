ExtIO_Pluto
======================================================================
ADLM PLUTO ExtIO DLL for HDSDR with basic functionality.

DLL uses AD IIO library & Pluto drivers. They must be preinstalled on the PC. Check IIO using IIO command line tools! 

For installation copy ExtIO_Pluto.DLL into HDSDR directory (default=C:\Program Files (x86)\HDSDR)
Select this lib in HDSDR, open ExIO window. Enter Pluto address (default=ip:192.168.2.1), press Set. Connect sucessful message must appear.
Restart HDSDR, set Sample rate (4000000 or less is good). Start receiver.
This lib is for unlocked version of Pluto, it supports 70MHz to 6HHz freqs. 
