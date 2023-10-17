# Webhook notification service interacting with cvmfs_ducc
Install and run as centos (from /home/centos):
```
sudo yum install -y python36-mod_wsgi python3-flask
sudo pip3 install python-dotenv
git clone --branch my-devel --depth 1 https://github.com/marcoverl/cvmfs.git
cd cvmfs/ducc/webhook/
#
# set variables in .env file, if you change the PROJECT_PATH var and/or the user, change registry-listener.service accordingly
#
mod_wsgi-express-3 setup-server registry_webhook.wsgi --port 8080 --user centos --server-root=mod_wsgi-express-8080/ --log-directory logs/ --access-log
sudo cp registry-listener.service /etc/systemd/system/; sudo systemctl daemon-reload
mod_wsgi-express-8080/apachectl start
sudo systemctl start registry-listener
#
#check logs here:
tail -f logs/error_log
journalctl -f -u registry-listener -l
```
To enable https with self-signed certificate:
```
sudo yum install -y mod_ssl
openssl genpkey -algorithm RSA -out server.key
openssl req -x509 -new -key server.key -out server.crt -days 365
mod_wsgi-express-3 setup-server registry_webhook.wsgi  --https-port 8080 --https-only --server-name cvmfs.wp6.cloud.infn.it --ssl-certificate-file server.crt --ssl-certificate-key-file server.key --user centos --server-root=mod_wsgi-express-8080/ --log-directory logs/ --access-log
```
