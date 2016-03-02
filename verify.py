#!/usr/bin/python

from PIL import Image


def check(palette, copy):
    palette = sorted(Image.open(palette).convert('RGB').getdata())
    copy = sorted(Image.open(copy).convert('RGB').getdata())
    if len(palette) != len(copy):
        return 'Images aren\'t the same size!'
    bad = 0
    for i in range(len(palette)):
        if copy[i] != palette[i]:
            bad += 1
    print 'Bad: {}/{}'.format(bad, len(palette))
    if bad:
        return 'Failed'
    return 'Success'

print check('palette.png', 'copy.png')

