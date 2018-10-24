
import argparse
import functools
import random
import phonenumbers 
from phonenumbers import carrier
from phonenumbers.phonenumberutil import number_type

a = functools.partial(random.randint, 0, 9)
gen = lambda: "+1{}{}{}{}{}{}{}{}{}{}".format(a(), a(), a(), a(), a(), a(), a(), a(), a(), a())

if __name__== "__main__":
    parser = argparse.ArgumentParser(description = 'Say hello')
    parser.add_argument('count', help='Enter Count')
    parser.add_argument('filename', help='Enter Filename')
    args = parser.parse_args()
    count = int(args.count)
    file = open(args.filename, "w") 
    while count != 0:
        number = gen()
        if carrier._is_mobile(number_type(phonenumbers.parse(number))) is True:
            count -= 1
            number = number[1:]
            file.write(number+'\n')
