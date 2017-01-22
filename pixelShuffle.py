from PIL import Image
from math import sqrt
import matplotlib.image as mpimg
import matplotlib.pyplot as plt
import numpy as np

F = np.array([0.114, 0.587, 0.299])


def pixelDiff(pix1, pix2):
    # return np.power(pix1[0] - pix2[0], 2) + np.power(pix1[1] - pix2[1], 2) + np.power(pix1[2] - pix2[2], 2)
    # return abs(pix1[0] - pix2[0]) + abs(pix1[1] - pix2[1]) + abs(pix1[2] - pix2[2])
    # return (pix1[0] - pix2[0])**2 + (pix1[1] - pix2[1])**2 + (pix1[2] - pix2[2])**2
    xi = pix1 * F
    yi = pix2 * F
    d = xi - yi
    # blah1 = np.sum(d)
    # blah2 = np.sum(xi) - np.sum(yi)
    t = np.sum(d * d)
    lx = np.sum(xi)
    ly = np.sum(yi)
    l = lx - ly
    # t = 0
    # lx = 0
    # ly = 0
    # for i in range(3):
    # 	xi = pix1[i] * F[i]
    # 	yi = pix2[i] * F[i]
    # 	d = xi - yi
    # 	t += d * d
    # 	lx += xi
    # 	ly += yi

    # l = lx - ly;
    return t + l * l


def swap(data, srcX, srcY, dstX, dstY):
    tmp = data[srcX, srcY]
    data[srcX, srcY] = data[dstX, dstY]
    data[dstX, dstY] = tmp


def totalDiff(data1, data2):
    return np.sum(pixelDiff(data1[y, x], data2[y, x]) for x in range(len(data1[0])) for y in range(len(data1)))


def determineSwap(s, d, sX, sY, dX, dY):
    return pixelDiff(s[sX, sY], d[dX, dY]) + pixelDiff(s[dX, dY], d[sX, sY]) < pixelDiff(s[sX, sY], d[sX, sY]) + pixelDiff(s[dX, dY], d[dX, dY])


def greatestDiff(srcImg, dstImg):
    best = 0
    bestX = 0
    bestY = 0
    for x in range(len(srcImg[0])):
        for y in range(len(srcImg)):
            tmp = pixelDiff(srcImg[y, x], dstImg[y, x])
            if tmp > best:
                best = tmp
                bestX = x
                bestY = y
    return bestX, bestY


def run(srcImg, dstImg):
    # srcImg *= 255
    # dstImg *= 255
    # srcImg.astype(int, copy=False)
    # dstImg.astype(int, copy=False)
    w = len(srcImg[0])
    h = len(srcImg)
    swaps = 0
    randomSwaps = 0
    iterations = int((w * h * (w / sqrt(2))) / 800)
    randomIterations = iterations

    print 'Width: {}'.format(w)
    print 'Height: {}'.format(h)
    print 'Total pixels: {}'.format(w * h)
    print 'Starting diff: {}'.format(totalDiff(srcImg, dstImg))

    for i in range(iterations):
        # if i % 2**15 == 0:
            # print '{:.3f}% done!'.format(float(i)/(iterations+randomIterations)*100)
        srcX = ((i / h) % h)
        srcY = ((i / w) % w)
        dstX = (i % h)
        dstY = (i % w)

        if determineSwap(srcImg, dstImg, srcX, srcY, dstX, dstY):
            swap(srcImg, srcX, srcY, dstX, dstY)
            swaps += 1

    for i in range(randomIterations):
        # if i % 2**15 == 0:
            # print '{:.3f}% done!'.format((float(i)+iterations)/(iterations+randomIterations)*100)

        srcX = np.random.randint(h)
        srcY = np.random.randint(w)
        dstX = np.random.randint(h)
        dstY = np.random.randint(w)

        if determineSwap(srcImg, dstImg, srcX, srcY, dstX, dstY):
            swap(srcImg, srcX, srcY, dstX, dstY)
            randomSwaps += 1

    # for i in range(iterations):
    # 	if i % 2**1 == 0:
    # 		print '{:.3f}% done!'.format(float(i)/iterations*100)
    # 	srcX, srcY = greatestDiff(srcImg, dstImg)
    # 	dstX = np.random.randint(h)
    # 	dstY = np.random.randint(w)
    # 	if determineSwap(srcImg, dstImg, srcX, srcY, dstX, dstY):
    # 		swap(srcImg, srcX, srcY, dstX, dstY)
    # 		swaps += 1

    print 'Ending diff: {}'.format(totalDiff(srcImg, dstImg))
    print 'Number of good swaps: {}'.format(swaps)
    print 'Number of bad swaps: {}'.format(iterations - swaps)
    print 'Percent good: {:.3f}%'.format(float(swaps) / iterations * 100)
    print 'Number of good swaps: {}'.format(randomSwaps)
    print 'Number of bad swaps: {}'.format(randomIterations - randomSwaps)
    print 'Percent good: {:.3f}%'.format(float(randomSwaps) / randomIterations * 100)

    # srcImg.astype(float, copy=False)
    # srcImg /= 255.0

monaNP = mpimg.imread('images/mona.png')
gothicNP = mpimg.imread('images/gothic.png')

run(monaNP, gothicNP)

plt.imshow(monaNP)
plt.show()
