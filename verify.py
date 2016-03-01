#!/usr/bin/python

from PIL import Image


def check(palette, copy):
    palette = sorted(Image.open(palette).convert('RGB').getdata())
    copy = sorted(Image.open(copy).convert('RGB').getdata())
    print 'Success' if copy == palette else 'Failed'

check('palette.png', 'copy.png')

