# Webhook notification service interacting with cvmfs_ducc
Install and run as centos (from /home/centos):
```
sudo yum install -y python36-mod_wsgi
sudo pip3 install python-dotenv
git clone --branch my-devel --depth 1 https://github.com/marcoverl/cvmfs.git
cd cvmfs/ducc/webhook/
#
# set variables in .env file, if you change the PROJECT_PATH var, change registry-listener.service accordingly
#
mod_wsgi-express-3 setup-server webhook.wsgi --port 8080 --user centos --server-root=mod_wsgi-express-8080/ --log-directory logs/ --access-log
sudo cp registry-listener.service /etc/systemd/system/; sudo systemctl daemon-reload
mod_wsgi-express-8080/apachectl start
sudo systemctl start registry-listener
#
#check logs here:
tail -f logs/error_log
journalctl -f -u registry-listener -l
```
