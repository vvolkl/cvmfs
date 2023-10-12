# Webhook notification service interacting with cvmfs_ducc

As root:
```
yum install  -y python36-mod_wsgi
pip3 install python-dotenv
cd /opt/
git clone --depth 1 https://github.com/marcoverl/cvmfs.git
cd cvmfs/ducc/webhook
#
# set variables in .env file, then e.g.:
mkdir /var/log/webhook
chmod g+w /var/log/webhook
# if you change the PROJECT_PATH var, change registry-listener.service accordingly
#
# if SElinux enabled:
semanage fcontext -a -t httpd_sys_rw_content_t  "/var/log/webhook"
restorecon -Rv /var/log/webhook
#
mod_wsgi-express-3 setup-server registry_webhook.wsgi --port 8080 --user apache --group root --server-root=/etc/mod_wsgi-express-8080 --log-directory /etc/httpd/logs/webhook --access-log
cp registry-listener.service /etc/systemd/system/; systemctl daemon-reload
/etc/mod_wsgi-express-8080/apachectl start
systemctl start registry-listener
#
# check logs here:
journalctl -f -u registry-listener -l
tail -f /etc/httpd/logs/webhook/error_log
```
Another option is to run the wsgi app as a process of an already running httpd service:
```
yum install  -y python36-mod_wsgi
pip3 install python-dotenv
cd /opt/
git clone --depth 1 https://github.com/marcoverl/cvmfs.git
cd cvmfs/ducc/webhook
#
# set variables in .env file, then e.g.:
mkdir /var/log/webhook
chmod g+w /var/log/webhook
# if you change the PROJECT_PATH var, change registry-listener.service and webhook.conf accordingly
#
# if SElinux enabled:
semanage fcontext -a -t httpd_sys_rw_content_t  "/var/log/webhook"
restorecon -Rv /var/log/webhook
#
cp webhook.conf /etc/httpd/conf.d/
cp registry-listener.service /etc/systemd/system/; systemctl daemon-reload
systemctl restart registry-listener httpd; systemctl status registry-listener httpd
#
#check logs here:
journalctl -f -u registry-listener -l
tail -f /etc/httpd/logs/wh_error_log

