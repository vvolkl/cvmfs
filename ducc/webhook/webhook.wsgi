import os
os.environ['NOTIFILE'] = 'notifications.txt'
os.environ['ROTATION'] = '100'
from registry_webhook import app as application
