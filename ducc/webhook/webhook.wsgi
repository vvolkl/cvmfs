import os
os.environ['NOTIFILE'] = os.environ.get('NOTIFILE','notifications.txt')
os.environ['ROTATION'] = os.environ.get('ROTATION','100')
from registry_webhook import app as application
